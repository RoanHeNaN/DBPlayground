# A composable storage model: ObjectStore / FileFormat / Codec

> Status: **design settled, unimplemented.** This refines the T5 "native
> `ColumnarTableSource`" placeholder in [`ColumnarTableSource.md`](ColumnarTableSource.md)
> into a composable model, and supersedes that section's naming.
> Companion layering (already built): the `ITableSource` read seam +
> `Chunk`/`Column`/`Value`/`Schema` (T0–T4). This doc adds the layers *below*
> `ITableSource` that make "where the bytes live", "how they're encoded", and
> "what the table format is" independently swappable.

## Goal

Design the abstractions once so that, later, whether a table's bytes live in
**memory / disk / S3**, which **encoding/compression** they use, and which
**file/table format** they follow are each *just another implementation of an
interface* — not a rewrite of the read/write flow.

## What real engines actually do (the model we align to)

The instinct to separate IO, encoding, and read-semantics is correct and
universal, but the layers are a **nested stack**, not three co-equal planes:

```
Catalog          name -> table metadata location     (Hive Metastore / REST catalog)
   │
Table Format     one table = metadata over many files: snapshots, schema
   │             evolution, partitioning, file-level pruning  (Iceberg / Delta)
   ▼             — does NOT define encoding; delegates to a file format
File Format       one file's physical layout + page index/stats,
   │             built out of Encoding + Compression   (Parquet / ORC / Avro)
   ▼
Object Store      byte-range IO: ReadAt(offset,len)    (mem / disk / S3)
```

- `arrow::fs::FileSystem` / DataFusion `ObjectStore` / ClickHouse `IDisk` — all
  the same **byte-range** Object Store layer.
- Parquet `Encoding` (PLAIN/RLE/DICTIONARY/DELTA) + `Compression` (ZSTD/…) and
  ClickHouse `ICompressionCodec` — two-level, **self-describing by an id** read
  back from page/block headers. That is the Encoding layer, and it lives *inside*
  the file format, not as a top-level peer.
- Iceberg is a **table** format that sits *above* a **file** format (Parquet) and
  delegates data-file reading to it. They are different layers; do not put a
  native file layout and Iceberg side by side under one interface.

We build bottom-up: Object Store first (the only truly orthogonal axis), then a
native File Format that composes Codecs, and leave Table Format (Iceberg-style
multi-file metadata) and Catalog for when multi-file/snapshot needs appear.

## The seam that stays: `ITableSource`

Unchanged from T0–T4. The query layer only ever sees this. Its implementations
are named by **source kind** (one axis), the way ClickHouse names `Storage<X>`
and DataFusion names `<X>Table` — never by internal mechanism (no
`FileFormatTableSource`). Two families coexist:

```
ITableSource
├── BTreeTableSource                         (was RowTableSource)
│      a B+Tree ACCESS METHOD: key-addressed, owns page store + index + row
│      codec as one bundle. NOT built from FileFormat/Storage — see below.
│
└── TableSource(IFileFormat, IStorage)       the file-based family; composition
        IStorage (byte-range):   MemStorage / LocalStorage / S3Storage
        IFileFormat:             NativeColumnarFileFormat   (column-major)
                                 NativeRowStoreFileFormat   (row file, Avro-like)
```

Swap the medium → swap the injected `IStorage`. Swap row↔columnar → swap the
`IFileFormat`. The query layer is untouched either way.

## Why the B+Tree is NOT an `IStorage` (settled, with the rejected alternative)

The composition is only worth having if it is **orthogonal**: any `IFileFormat` ×
any `IStorage`. Orthogonality holds only when `IStorage` is a *single addressing
model*. B+Tree and files use different ones:

| source                         | addressed by | primitive                        |
|--------------------------------|--------------|----------------------------------|
| columnar file / row file       | byte offset  | `ReadAt(offset, len) -> bytes`   |
| B+Tree / LSM                   | key          | `Get(key)`, ordered `Scan(range)`|

A single `IStorage` covering both becomes the union `{ReadAt+Size} ∪ {Get+ordered
cursor}`, where **every implementation supports only half**: a B+Tree store
can't serve arbitrary `ReadAt` (its bytes are pages behind a buffer pool + index,
not a flat addressable stream), and an S3 store can't serve `Get(key)`/ordered
KV. Then `NativeColumnarFileFormat` can't pair with a B+Tree store and
`NativeRowStoreFileFormat` can't pair with S3 — the two "axes" are locked
together, so the orthogonality is fake. Two things that always appear as a fixed
pair should be **one** thing, not two injected axes (interface-segregation).

Layer-wise: a B+Tree is an **access method** — it already bundles page store +
index + record format. It sits at the *same* level as `TableSource`, not below
it (this is why today's `RowTableSource` wraps the engine directly). ClickHouse
mirrors this: `StorageMergeTree` (indexed access method) and `StorageFile`
(byte-range file) are *sibling* `IStorage` implementations, not two products of
one format+medium composition.

**Rejected:** making `IStorage` a KV/key-addressed interface so the B+Tree fits.
Cost: S3/columnar then needs byte-range re-introduced *below* `IStorage` as yet
another layer, B+Tree and S3 stop being peers, and the model turns circular.

**Corollary — which "row store" unifies:** a *row file* (rows serialized
sequentially into a file, no index — the Avro/heap-file analog) is byte-range
addressed and fits perfectly as `NativeRowStoreFileFormat` over any `IStorage`
(mem/disk/S3). A *B+Tree indexed* row store does not — it is an access method,
not a byte-range medium. "row store" was ambiguous between these two.

## Interfaces (sketch, `namespace dbplay`)

Virtual at the seams (implementations are chosen at runtime and self-described by
ids read from metadata — templates can't select an impl from a persisted id, and
would explode into N formats × M media × K codecs). Templates only inside a
codec's per-`Type` kernel.

**Error convention** (matches the repo — there is no `Status` type): `bool` +
out-pointer for expected sad paths (as `IStorageEngine::Get`), a null
`unique_ptr` for "not found", and `throw std::runtime_error` for hard IO errors
(as `DiskManager`). Owning output goes through `std::string *` since `Slice` is a
non-owning view.

### Layer A — Object Store (byte-range medium)  [`Storage/File/`]

```cpp
class IInputFile {                                   // random-access read
 public:
  virtual ~IInputFile() = default;
  virtual uint64_t Size() const = 0;
  // Read [offset, offset+len) into *out (out owns the bytes). False if the range
  // lies outside the file; throws std::runtime_error on an IO error.
  virtual bool ReadAt(uint64_t offset, size_t len, std::string *out) const = 0;
};

class IOutputStream {                                // sequential append
 public:
  virtual ~IOutputStream() = default;
  virtual void Append(const Slice &data) = 0;        // throws on IO error
  virtual void Close() = 0;                           // commit; idempotent
};

class IStorage {                                     // aka ObjectStore
 public:
  virtual ~IStorage() = default;
  // Open for random reads; nullptr if `path` does not exist.
  virtual std::unique_ptr<IInputFile>    OpenInput (const std::string &path) = 0;
  // Open for sequential writing, truncating any existing content.
  virtual std::unique_ptr<IOutputStream> OpenOutput(const std::string &path) = 0;
  virtual bool                     Exists(const std::string &path) const = 0;
  virtual std::vector<std::string> List  (const std::string &prefix) const = 0;
  virtual void                     Delete(const std::string &path) = 0;
};
// impls: MemStorage, LocalStorage, (future) S3Storage
```

### Layer B — Codec (encoding + compression), inside the file format

```cpp
//  values --Encode--> encoded bytes --Compress--> stored bytes   (Parquet's two levels)
class ICodec {                                       // per-column, resolved by id
 public:
  virtual EncodingId id() const = 0;
  virtual void Encode(const Column &col, std::string *out) = 0;
  virtual void Decode(const Slice &bytes, size_t value_count, Column *out) = 0;
};

class ICompression {                                 // None / Zstd / LZ4 / zlib(contrib)
 public:
  virtual CompressionId id() const = 0;
  virtual void Compress  (const Slice &in, std::string *out) = 0;
  virtual void Decompress(const Slice &in, size_t raw_size, std::string *out) = 0;
};

class CodecRegistry {                                // id (from page header) -> codec
 public:
  const ICodec       &Get(EncodingId) const;
  const ICompression &Get(CompressionId) const;
};
```

A codec's hot path dispatches once on `Type`, then runs a monomorphized kernel:

```cpp
template <class T> void PlainDecodeKernel(Slice in, size_t n, Column *out);
void PlainCodec::Decode(const Slice &in, size_t n, Column *out) {
  switch (out->type()) {
    case Type::Int32: PlainDecodeKernel<int32_t>(in, n, out); break;
    /* ... */
  }
}
```

### Layer C — File Format + the composed `TableSource`

```cpp
class IFileFormat {                                  // carries row-vs-columnar + decode
 public:
  virtual ~IFileFormat() = default;
  virtual const Schema &schema() const = 0;
  // Projection pushdown happens here: a columnar format reads only the
  // projected column streams; a row-file format reads whole records.
  virtual std::unique_ptr<IBatchCursor> Scan(
      IStorage &store, const std::vector<std::string> &files,
      const std::vector<int> &projection) = 0;
  virtual std::unique_ptr<IChunkWriter> OpenWriter(IStorage &store, const std::string &path) = 0;
};

class TableSource : public ITableSource {            // the composition; no row/col subclass
 public:
  TableSource(std::shared_ptr<IFileFormat> fmt, std::shared_ptr<IStorage> store,
              std::vector<std::string> files)
      : fmt_(std::move(fmt)), store_(std::move(store)), files_(std::move(files)) {}
  const Schema &schema() const override { return fmt_->schema(); }
  std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &proj) override {
    return fmt_->Scan(*store_, files_, proj);        // thin delegation
  }
 private:
  std::shared_ptr<IFileFormat> fmt_;
  std::shared_ptr<IStorage>    store_;
  std::vector<std::string>     files_;
};
```

Honest note: the "unified read/write" is unification at the **interface** (all
paths yield `Chunk` cursors), not one shared algorithm body — columnar and
row-file scans differ internally. `TableSource` mostly delegates; the only truly
shared logic is batch/`Chunk` assembly, which can be a small helper. `ICodec` is
a **per-column, format-internal** construct (a columnar format resolves one per
column by id), not a member the row path shares.

### Native columnar file layout (Parquet-lite)

```
File = [Header]
       [Row Group 0 .. N]                    horizontal row slices -> batch/parallel
         Column Chunk 0 .. M                 each column contiguous -> projection reads only needed columns
           Page 0 .. K                       Page = the encoding + compression + IO unit
             [PageHeader: encoding_id, compression_id, value_count, sizes]
             [payload]
       [Footer: schema + per-(row-group,column) offset+length + stats]
```

Index/statistics are a **first-class** part of the file format (Parquet page
index, ORC row-index, MergeTree marks), not a footnote: the footer stats drive
row-group skipping (predicate pushdown), the offsets drive projection reads.

## Build phases (each a testable increment, T0–T4 style)

- **C1 — Object Store** — `IStorage`/`IInputFile`/`IOutputStream` + `MemStorage`
  + `LocalStorage`; range read/append round-trip tests.
- **C2 — Codec** — `ICodec` (Plain) + `ICompression` (None, then contrib zlib) +
  `CodecRegistry`; column round-trip tests.
- **C3 — Native columnar file + composition** — `NativeColumnarFileFormat`
  (writer + reader with footer/pages), the composed `TableSource`; write→read a
  file through Layer A/B, projection-pushdown test against `ITableSource&`.
- **C4 — pushdown & more impls** — footer-stats predicate pushdown; Dictionary/RLE
  codecs; `NativeRowStoreFileFormat`; S3Storage stub. All additive.
- **Refactor (independent)** — fold `IStorageEngine`/`BPlusTreeEngine` into
  `BTreeTableSource`; consumers to migrate: `MiniKV`, `RowTableSource`,
  `KvCursorTest`.

## Decisions (settled) and still open

Settled here:
1. `ITableSource` impls named by source kind on one axis; `TableSource` is the
   composed file-based family; `BTreeTableSource` is a sibling access method.
2. `IStorage` = byte-range medium only (single addressing model → real
   orthogonality). B+Tree is an access method, not an `IStorage`.
3. Two-level, self-describing (encode→compress, ids in headers); virtual at the
   seams, templates only in codec kernels.
4. Index/stats are part of the file format from the start.

Open / TODO:
- **Nulls / validity bitmap** in `Column` and its def-level cost in codecs
  (deferred in T1; will touch the codec interface — leave room now).
- **Table Format layer** (Iceberg-style multi-file metadata/snapshots) and a
  **Catalog** (name → table location/schema, also needed to reopen a table).
- Concrete `ICompression` (zlib is already in `contrib`), Dictionary/RLE/Delta
  encodings, `S3Storage`.
