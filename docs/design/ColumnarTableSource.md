# Supporting Columnar Engines: the ITableSource seam

> Status: **T0–T4 implemented** (the row path works end to end through the seam).
> T5 (a native columnar engine) is future. See the phase list at the bottom.
> Companion: [`StorageEngineRefactor.md`](StorageEngineRefactor.md) built the
> type-erased **KV / row** engine (`IStorageEngine`, `Slice` in / bytes out).
> This doc adds the layer that lets a **columnar** engine coexist with it.

## The problem this solves

The `IStorageEngine` contract (`Insert(Slice key, Slice value)` / `Get -> bytes`)
is a **row / KV** contract: the value is one opaque blob and the engine is
schema-agnostic by design. "Multi-column" = the application serializes all
columns into that blob = **row storage**.

A **columnar** engine cannot live under that contract: it must *understand the
schema* to store each column separately, compress per column, and read only the
projected columns. Schema-awareness is exactly what the KV seam erases.

Conclusion: row and columnar engines need **different seams**. We keep the KV
seam for row/KV engines, and add a higher, schema-aware, vectorized seam —
`ITableSource` — that both kinds of engine can implement. This is also the
"storage ↔ query engine mediator" the project originally wanted (the once-
deferred iterator), upgraded to be schema-aware and batch-oriented.

## Layering (as built)

```
                 Query / executor layer
                 CollectProjected(ITableSource&, projection)   [Execution/ScanExecutor]
                        │  Scan(projection) -> Chunks
                        ▼
   ┌───────────────────────────────────────────────────────────┐
   │ ITableSource / IBatchCursor   [Table/ITableSource.h]       │  ← the mediator
   │   Schema (Field) + Chunk (column-major)                    │
   └───────────────┬───────────────────────────────┬───────────┘
                   │ row-store adapter               │ columnar native (future)
                   ▼                                 ▼
        RowTableSource  [Table/RowTableSource]   ColumnarTableSource (T5)
          Schema + RowCodec + IKvCursor            stores/reads by column,
                   │ uses                            projection/filter pushdown
                   ▼
        IStorageEngine (Slice KV)   ← unchanged; BPlusTreeEngine (+ NewCursor)
```

The query layer talks **only** to `ITableSource`; `ScanExecutorTest` builds a
`RowTableSource` but hands the operator an `ITableSource&`, proving the
decoupling. A native `ColumnarTableSource` drops in under the same interface
with the query layer unchanged.

## Types actually built

### Schema (the "column description")

```cpp
struct Field { std::string name; Type type; };   // Table/Schema.h
using Schema = std::vector<Field>;
```

Named `Field` (Arrow's term) so the *data* column can be `Column` without a
name clash.

### Column — one column's batch of data (Arrow-lite)  [Table/Column.h]

Column-major, contiguous per column. Fixed-width types are packed; variable-
length (String/Blob) use `offsets` + a shared `var_bytes` buffer. **No validity
bitmap (nulls) in this first cut.** `Append<T>` / `AppendBytes` to build,
`Get<T>` / `GetBytes` to read.

### Chunk — one batch of rows, column-major  [Table/Chunk.h]

```cpp
struct Chunk {
  std::vector<int> column_ids;   // projected schema indices, parallel to columns
  std::vector<Column> columns;
  size_t row_count = 0;
};
```

A ClickHouse/DuckDB-style **Chunk** (data + which fields), not a self-describing
"Block"; names/types come from `ITableSource::schema()`.

### Value — one typed row cell  [Table/Value.h]

Used to build a row for `RowCodec::Encode` and to hand back decoded rows / cells.

## The mediator interface  [Table/ITableSource.h]

```cpp
class IBatchCursor {
 public:
  virtual ~IBatchCursor() = default;
  virtual bool Next(Chunk *out) = 0;   // false when exhausted
};

class ITableSource {
 public:
  virtual ~ITableSource() = default;
  virtual const Schema &schema() const = 0;
  // Projection pushdown: schema field indices to materialize, in output order.
  virtual std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) = 0;
};
```

Filter pushdown / statistics / ordering are **not** here yet; they will be added
as optional capability interfaces the query layer probes for (see Future).

## How the row/KV engine plugs in (implemented)

1. **KV ordered cursor** [`Storage/IKvCursor.h`, `IStorageEngine::NewCursor()`]:
   `BPlusTreeEngine` walks the B+Tree leaf chain (`BPlusTree::FirstLeafPageId()`
   + leaf `NextPageId`) and fetches each value from the `TupleStore`, yielding
   ordered `(Slice key, Slice value)`.
2. **RowCodec** [`Table/RowCodec.{h,cpp}`]: schema-aware, in the table layer.
   Blob format = fields in schema order; fixed-width inline, var-len as
   `uint32 length + bytes`. `Encode(row)`, `Decode(blob)`, and
   `DecodeInto(blob, projection, cols)` which materializes only the projected
   fields into columns.
3. **RowTableSource** [`Table/RowTableSource.{h,cpp}`]: `Scan(projection)`
   returns a cursor that pulls up to `batch_rows` rows from the KV cursor,
   `DecodeInto`s each row blob for the projected fields, and emits `Chunk`s.

So a schema-agnostic KV engine becomes a projectable, batch-yielding table by
row→column materialization, reusing Phases 0–4 unchanged. (Convention: the KV
value blob holds the whole row per `schema`; the KV key is the encoded primary
key.)

## How a native columnar engine plugs in (future, T5)

`ColumnarTableSource` implements `ITableSource` directly: each column in its own
page chain, `Scan(projection)` reads only the requested column chains and emits
`Chunk`s natively — never touching the KV seam. It does not reuse
`IStorageEngine`; it is a sibling under `ITableSource`.

## Build phases

- **T0 — KV ordered cursor** — DONE (`IKvCursor`, `IStorageEngine::NewCursor()`,
  `BPlusTree::FirstLeafPageId()`, `BPlusTreeEngine` leaf-chain cursor;
  `KvCursorTest`).
- **T1 — schema + batch types** — DONE (`Field`/`Schema`, `Column`, `Chunk`;
  `ColumnChunkTest`).
- **T2 — RowCodec** — DONE (`Value`, `RowCodec`; `RowCodecTest`).
- **T3 — ITableSource + RowTableSource** — DONE (projection pushdown;
  `RowTableSourceTest`).
- **T4 — a scan→project→collect operator** — DONE (`Execution/ScanExecutor`;
  `ScanExecutorTest` runs against `ITableSource&` only).
- **T5 — native ColumnarTableSource** — FUTURE.

T0–T4 were almost entirely **additive**: the only change to existing code was
`IStorageEngine` gaining `NewCursor()` (+ `BPlusTree` a couple of accessors).
The KV engine, B+Tree, TupleStore and catalog stay as the row backend.

## Decisions (settled) and what is still open

Settled during T0–T4:
1. **Arrow-lite column layout, no nulls** in the first cut (validity bitmap
   deferred).
2. **Projection pushdown implemented; filters not.** Filter pushdown /
   statistics will be optional capability interfaces (probe via `dynamic_cast`),
   added when needed.
3. **RowCodec format**: length-prefixed var-len, fixed-width inline, schema order.

Still open / TODO:
- **Persist the table `Schema`.** `RowTableSource` currently takes the schema in
  memory; the Phase 4 `MetaPage` still stores only a single key/value `Type`. To
  reopen a *table* and scan it, the `Schema` must be persisted (extend `MetaPage`
  or add a catalog page).
- **Nulls / validity bitmap** in `Column`.
- **T5 native columnar engine**, and filter/statistics pushdown capabilities.
- Unrelated carry-overs from the KV work: small-pool OOM hardening; variable-
  length keys; cleaning up `Config.h`'s `key_t/value_t/values`.
