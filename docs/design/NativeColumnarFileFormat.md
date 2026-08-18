# NativeColumnarFileFormat — on-disk file layout (`DBC1`)

> Status: **implemented (C3/C4)**. This is the byte-exact spec of the file that
> `NativeColumnarWriter` writes and `NativeColumnarCursor` reads
> (`src/Table/Format/NativeColumnarFileFormat.cpp`), the column encoding from
> `PlainCodec` (`src/Storage/Encoding/PlainCodec.cpp`), and the codec ids from
> `src/Storage/Encoding/Codec.h`.
> Context: this is the `IFileFormat` half of the composable model in
> [`StorageAbstraction.md`](StorageAbstraction.md); it reads/writes through an
> `IStorage` byte-range medium and produces `Chunk`s for the `ITableSource` seam.

## Scope of the current format

- **One file = one row group.** Each column is stored **contiguously as a single
  page** (whole column buffered by the writer and sealed on `Close()`).
- Encoding is **Plain**; compression is per-file-instance (`None` or `Zlib`) and
  **self-described per column** in the footer, so a reader resolves the codec
  from the file, not from how the format was configured.
- A scan reads **only the projected columns'** byte ranges (projection pushdown).
- **Not yet** (aspirational in `StorageAbstraction.md`, not in the bytes below):
  multiple row groups, multiple pages per column, page/footer **statistics**,
  and **schema persisted in the file** (the reader gets the `Schema` from the
  `NativeColumnarFileFormat` instance — persisting it is the catalog TODO). The
  evolution to a positional `ReadRange(first_row, num_rows, projection)` read
  path, per-column direct-offset vs page-decode, and row groups + page index is
  designed in [`ColumnarReadPath.md`](ColumnarReadPath.md).

## Conventions

- All multi-byte integers are written by raw `memcpy` of the native
  representation → **host-endian** (little-endian on the supported x86-64/arm64
  platforms). `u8` = 1 byte, `u64` = 8 bytes.
- Offsets in the footer are **absolute** file offsets.
- Magic: the 4 ASCII bytes `DBC1` (`{'D','B','C','1'}`), written at **both** the
  start and the very end of the file. (The reader validates the *trailing*
  magic; the leading magic is currently informational.)

## Top-level layout

```
offset 0                                              file
┌───────────────────────────────────────────────────────────────────────┐
│ magic "DBC1"                                       4 bytes              │
├───────────────────────────────────────────────────────────────────────┤
│ column-page region:                                                     │
│   column 0 page  = Compress(Encode(column 0))                           │
│   column 1 page  = Compress(Encode(column 1))                           │
│   ...                                                                   │  each column's page located
│   column M-1 page                                                       │  by footer {offset, stored_size}
├───────────────────────────────────────────────────────────────────────┤
│ FOOTER  (footer_size bytes):                                            │
│   row_count           u64                                               │
│   col_count           u64                                               │
│   ColumnMeta[0]       34 bytes                                          │
│   ColumnMeta[1]       34 bytes                                          │
│   ...                                                                   │
│   ColumnMeta[col_count-1]                                               │
├───────────────────────────────────────────────────────────────────────┤
│ footer_size           u64   (length of the FOOTER block above)          │
│ magic "DBC1"          4 bytes                                           │
└───────────────────────────────────────────────────────────────────────┘
                                                            end of file
```

Fixed sizes: leading magic = 4; `footer_size` field + trailing magic (the
"trailer") = 8 + 4 = 12; `footer_size = 16 + 34 * col_count`.

### ColumnMeta (34 bytes, one per column, in schema/field order)

| field         | type | bytes | meaning                                             |
|---------------|------|-------|-----------------------------------------------------|
| `encoding`    | u8   | 1     | `EncodingId` (Plain = 1)                            |
| `compression` | u8   | 1     | `CompressionId` (None = 1, Zlib = 2)                |
| `offset`      | u64  | 8     | absolute file offset of this column's stored page   |
| `stored_size` | u64  | 8     | page length on disk (compressed)                    |
| `raw_size`    | u64  | 8     | encoded length before compression (sizes decompress)|
| `value_count` | u64  | 8     | number of values in the column                      |

`value_count == row_count` for every column today (single row group), but it is
stored per column so multi-page / ragged layouts remain possible later.

## Inside a column page

`page = Compress(Encode(column))`, two self-describing levels (Parquet-style):

**Level 1 — encoding (Plain).** Appends every value; dispatch on column `Type`:
- fixed-width (`Int32`=4, `Int64`=8, `Float`=4, `Double`=8, `Bool`=1): the raw
  value bytes, packed contiguously (`value_count * sizeof(T)` total).
- variable-length (`String`/`Blob`): per value a `u32 length` then that many
  payload bytes.

**Level 2 — compression.** `None` = identity (stored == encoded); `Zlib` =
deflate over the encoded bytes (`raw_size` tells the reader the decompressed
length up front). The reader picks the decompressor and decoder purely from the
`compression`/`encoding` ids in the `ColumnMeta` — that is what makes a
`None`-configured format able to read a `Zlib`-written file.

## Read path (`NativeColumnarCursor`)

Per file (each file yields one `Chunk`; empty files — `row_count == 0` — are
skipped):
1. `size = in.Size()`. Read the last 4 bytes; verify they equal `DBC1`.
2. Read `footer_size` = the `u64` at `size - 12`.
3. Read the footer at `[size - 12 - footer_size, size - 12)`; parse `row_count`,
   `col_count`, and the `ColumnMeta` array.
4. For each **projected** field index: `ReadAt(meta.offset, meta.stored_size)` →
   `Decompress(raw_size)` → `Decode(value_count)` into a `Column` of
   `schema[field].type`; push into the output `Chunk`. Non-projected columns are
   never read → projection pushdown at the IO layer.

## Write path (`NativeColumnarWriter`)

Buffers full columns across `Write(Chunk)` calls (chunks must be full-schema),
then on `Close()`: write leading magic; for each column encode→compress, record
its `ColumnMeta` (offset = current body length), append the page; append the
footer; append `footer_size`; append trailing magic; flush to the `IOutputStream`.

## Worked example

Schema `(a Int32, b String)`, compression `None`, two rows `(1,"hi")`,`(2,"xyz")`:

```
off  bytes                                   meaning
0    44 42 43 31                             magic "DBC1"
4    01 00 00 00  02 00 00 00                col a page: int32 1, int32 2   (8B)
12   02 00 00 00 "hi" 03 00 00 00 "xyz"      col b page: (len2)"hi"(len3)"xyz" (13B)
25   02 00 00 00 00 00 00 00                 footer.row_count = 2
33   02 00 00 00 00 00 00 00                 footer.col_count = 2
41   01 01  04.. 08.. 08.. 02..              ColumnMeta[a]: enc1 comp1 off4 stored8 raw8 vc2
75   01 01  0C.. 0D.. 0D.. 02..              ColumnMeta[b]: enc1 comp1 off12 stored13 raw13 vc2
109  54 00 00 00 00 00 00 00                 footer_size = 84
117  44 42 43 31                             trailing magic "DBC1"
121  <eof>
```

`footer_size = 16 + 34*2 = 84`; file size `= 4 + 8 + 13 + 84 + 12 = 121`.

## Invariants / notes

- Minimum valid file: `4 (leading magic) + 12 (trailer)`; the reader also
  requires the footer to fit. An empty table still writes a valid file with
  `row_count = 0` and `col_count = schema size`.
- The footer does **not** store field names/types; the reader must be given the
  matching `Schema` via the format. Persisting the schema (and multi-file table
  metadata) is the catalog work noted in `StorageAbstraction.md`.
- Because column pages are located by absolute `offset` in the footer, the
  leading magic is not needed for correctness — it is a human/`file(1)`-friendly
  marker and a placeholder for a future header (version, flags).
```
