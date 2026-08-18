# Columnar read path: positional ReadRange + a uniform decode pipeline

> Status: **design proposal** (evolves the implemented format). Today
> `NativeColumnarFileFormat` is one row group, one page per column, and a scan
> reads the *whole* column into one `Chunk`
> ([`NativeColumnarFileFormat.md`](NativeColumnarFileFormat.md)). This doc derives,
> from first principles, the read interface a data file *should* expose and the
> internal structure that makes it efficient, then lays out incremental phases.
> Companion: [`StorageAbstraction.md`](StorageAbstraction.md) (the ObjectStore /
> FileFormat / Codec layering); the `Chunk`/`Column` types are from T1.
> ("Block" in the discussion = our `Chunk`.)

## First principle: what a data file must offer

A scan wants to read **some rows, of some columns, in batches, skipping what it
doesn't need**. Reduced to its essence, a file's read primitive is positional
and projected:

```
ReadRange(first_row, num_rows, projection) -> Chunk
```

- Addressing unit = the **logical row number** within the file. The footer's job
  is to translate a row number into a physical location.
- It subsumes today's "whole file" (`ReadRange(0, N, proj)`), and directly gives
  **batching** (a cursor is repeated `ReadRange` by `batch_rows`), **range scans**,
  and a hook for **row-group skipping** (just don't `ReadRange` a skipped range).

The query-layer seam stays the streaming cursor (`ITableSource::Scan(projection)`
→ `IBatchCursor::Next(Chunk*)`, from T0–T4); `ReadRange` is the **internal
primitive** the cursor is built on (the executor wants "feed me batches", not to
compute offsets). Predicate pushdown = the cursor skips row-group windows whose
stats exclude the predicate.

## The uniform decode pipeline (one column, one row range)

Independent of layout and codec, reading one column over a row range is always
five steps:

```
Locate      row range  ->  the page(s) covering it (byte ranges + each page's first_row)   [footer index]
Fetch       IStorage.ReadAt(offset, len)  ->  stored bytes
Decompress  ICompression.Decompress(id)   ->  encoded bytes        [id from page/footer]
Decode      ICodec.Decode(id)             ->  values               [id from page/footer]
Slice       take [first_row - page.first_row, +num_rows) from the decoded page(s)
```

- **Row groups / pages** are only what `Locate` consumes; **encoding/compression**
  are only self-describing parameters of `Decompress`/`Decode`.
- Projection runs this pipeline **per projected column independently** (each
  column has its own pages and its own codec), then assembles the `Chunk`. So the
  page-vs-offset decision below is made **per column**.

## The real subtlety: random access is not free — page-decode vs direct-offset

The decodable unit is the **page**: compressed streams, variable-length
(offsets), RLE / dictionary / delta all must be decoded **from a page boundary**
— you cannot jump to row *i* in the middle. So the general path is: `Locate`
rounds `first_row` **down to the page that contains it**, the pipeline decodes
that page, and `Slice` cuts out the wanted rows.

There is exactly one shortcut — read only the needed bytes without decoding the
whole page — and it is a **per-column capability**, not a special case to sprinkle
through the reader:

```
direct-offset random read   ⇔   codec.is_fixed_width()  ∧  compression == None
```

When a column's page is **fixed-width Plain and uncompressed**, value *i* lives at
`page_data + i * width`, so `ReadAt(offset + first*width, num*width)` fetches
exactly the range and decodes only it. Otherwise (var-len / compressed / RLE /
dict) → **page-decode-and-slice**. Model it as a capability on the codec:

```cpp
class ICodec {
 public:
  // ... id(), Encode(), Decode() ...
  // Bytes per value if this encoding is fixed-width and directly addressable
  // (Plain int/float/bool); 0 for variable-length / non-addressable encodings.
  virtual size_t FixedWidth(Type t) const = 0;
};
```

The reader chooses the path once per (column, page): `FixedWidth(type) > 0 &&
compression == None` → direct-offset; else page-decode. No `switch` on concrete
encodings in the reader.

## What row groups + a page index buy

Introduce **multiple row groups**, and in the footer a **page index** per
`(row group, column)` recording each page's `first_row`, `offset`,
`stored_size`, `raw_size`, `value_count`, `encoding`, `compression` (and later
min/max **stats**). Then:

- `Locate(row range)` maps to just the pages spanning the range → random access
  decodes **one page**, not the whole column (bounded CPU/memory).
- Row group / page is the **skipping granularity**: with per-page stats, a
  predicate that can't match a page means `Locate` omits it — the pipeline never
  runs for it. (Predicate evaluation itself belongs to the expression engine,
  [`ExpressionEngine.md`](ExpressionEngine.md), not ad-hoc here.)

Today's "single row group, one page per column" is the degenerate case: `Locate`
returns the whole-column page; `ReadRange` decodes it and slices.

## Interfaces (sketch)

```cpp
// Internal file primitive (columnar reader); the ITableSource cursor wraps it.
class NativeColumnarReader {
 public:
  uint64_t row_count() const;
  // Fill *out with rows [first_row, first_row+num_rows) of the projected columns.
  // Clamps num_rows to the file end; false if the range is empty.
  bool ReadRange(uint64_t first_row, uint64_t num_rows,
                 const std::vector<int> &projection, Chunk *out);
};
```

Footer page-index entry (per row group, per column):

```
first_row  u64 | offset u64 | stored_size u64 | raw_size u64 | value_count u64
encoding   u8  | compression u8 | (later) stats {min,max,null_count}
```

`Locate` = binary search these by `first_row` to find the pages overlapping the
requested range.

## Build phases (each a testable increment)

- **R1 — positional primitive + batched cursor (no layout change).**
  Add `ReadRange(first_row, num_rows, projection)` over the current single-page
  layout (decode the column page, `Slice` the range) and make the `ITableSource`
  cursor stream `batch_rows`-sized `Chunk`s via repeated `ReadRange`. Gains range
  scans + bounded **output** batches. (Decode is still whole-page here — bounded
  *decode* comes in R3.) Test: `ReadRange` sub-ranges and batched full scan equal
  the whole-file read.
- **R2 — codec random-access capability + direct-offset fast path.**
  Add `ICodec::FixedWidth(Type)`; for fixed-width + uncompressed columns,
  `ReadRange` does a **partial page read** (`ReadAt` only the needed values) and
  decodes only those. Test: fixed-width column range read touches only the
  expected byte sub-range (spy `IStorage`), var-len/compressed falls back to
  page-decode, results identical.
- **R3 — multiple row groups + footer page index.**
  Writer flushes a row group every `rows_per_group`; footer gains the page index
  (`first_row`/offsets per (row group,column)). `Locate` reads only the pages
  overlapping the range → **bounded decode** and true random access. Test:
  random `ReadRange`s across many row groups; assert only the covering pages are
  fetched.
- **R4 — per-page stats + skipping (deferred to the expression engine).**
  Footer stats (min/max/null_count) per page; the cursor skips pages a predicate
  excludes. Predicate representation/evaluation comes from
  [`ExpressionEngine.md`](ExpressionEngine.md); this phase only wires the
  skip-list into `Locate`.

R1/R2 change no file bytes (pure reader/cursor evolution); R3 changes the footer
(new format version — bump the magic or a version field in the header). All are
additive to the `ITableSource` seam — the query layer is untouched.

## Decisions (proposed) and open

Proposed:
1. File primitive is **positional** `ReadRange(first_row, num_rows, projection)`;
   the public seam stays a streaming cursor built on it.
2. "page-decode vs direct-offset" is a **per-column codec capability**
   (`FixedWidth`), gated by `compression == None`; not encoding-specific branches
   in the reader.
3. Row group + footer page index are the granularity for both bounded random
   access and predicate skipping; introduced together in R3.

Open:
- **Bounded decode before R3**: until row groups exist, `ReadRange` must decode a
  whole column page even for a small range (var-len/compressed). Acceptable
  interim; R3 fixes it.
- Nulls/validity interact with `value_count` vs row positions (def-levels); still
  deferred (see `StorageAbstraction.md`).
- Choosing `rows_per_group` / page size (a size vs. skip-granularity trade-off).
