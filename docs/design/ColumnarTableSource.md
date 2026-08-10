# Supporting Columnar Engines: the ITableSource seam

> Status: design proposal (direction chosen: make the architecture columnar-ready).
> Companion: [`StorageEngineRefactor.md`](StorageEngineRefactor.md) built the
> type-erased **KV / row** engine (`IStorageEngine`, `Slice` in / bytes out).
> This doc adds the layer that lets a **columnar** engine coexist with it.

## The problem this solves

The `IStorageEngine` contract (`Insert(Slice key, Slice value)` / `Get -> bytes`)
is a **row / KV** contract: the value is one opaque blob and the engine is
schema-agnostic by design. "Multi-column" today = the application serializes all
columns into that blob = **row storage**.

A **columnar** engine cannot live under that contract: it must *understand the
schema* to store each column separately, compress per column, and read only the
projected columns. Schema-awareness is exactly what the KV seam erases.

Conclusion: row and columnar engines need **different seams**. We keep the KV
seam for row/KV engines, and add a higher, schema-aware, vectorized seam —
`ITableSource` — that both kinds of engine can implement. This is also the
"storage ↔ query engine mediator" the project originally wanted (the deferred
iterator), upgraded to be schema-aware and batch-oriented.

## Layering

```
                 Query / executor layer
                        │  Scan(table, projection[, filters]) -> column batches
                        ▼
   ┌───────────────────────────────────────────────────────────┐
   │ ITableSource / IBatchCursor  (schema-aware, VECTORIZED)     │  ← the mediator
   │   Schema  +  ColumnBatch (column-major)                     │
   └───────────────┬───────────────────────────────┬───────────┘
                   │ row-store adapter               │ columnar native
                   ▼                                 ▼
        RowTableSource                        ColumnarTableSource (future)
          (Schema + RowCodec +                  (stores/reads by column,
           KV scan iterator)                     projection/filter pushdown)
                   │ uses
                   ▼
        IStorageEngine (Slice KV)   ← unchanged; BPlusTreeEngine / future LSM
```

Key point: `RowTableSource` turns a row/KV engine into a table by decoding each
row blob into the requested columns and packing them into column batches.
`ColumnarTableSource` skips the KV seam entirely and produces batches natively.
The query layer talks **only** to `ITableSource` and does not know which it got.

## New types (schema + vectorized batch)

Schema reuses the existing `Type` enum:

```cpp
struct Column { std::string name; Type type; };
using Schema = std::vector<Column>;
```

A column batch is **column-major** (that is what makes columnar fast and is what
a native columnar engine emits directly). Each column is an Arrow-lite vector:
fixed-width types are packed; variable-length (String/Blob) use an offsets +
bytes pair; a validity bitmap carries nulls.

```cpp
class ColumnVector {
 public:
  Type type() const;
  size_t size() const;                 // number of rows in this batch
  bool is_null(size_t i) const;        // validity bitmap

  // Fixed-width access (Int32/Int64/Float/Double/Bool): typed view over packed bytes.
  template <typename T> T Get(size_t i) const;

  // Variable-length access (String/Blob): a Slice into the shared bytes buffer.
  Slice GetBytes(size_t i) const;

 private:
  Type type_;
  size_t size_;
  std::vector<uint8_t> validity_;      // 1 bit / row
  std::vector<char> fixed_;            // fixed-width packed values
  std::vector<uint32_t> offsets_;      // var-len: offsets_[i]..offsets_[i+1]
  std::vector<char> var_bytes_;        // var-len payload
};

struct ColumnBatch {
  std::vector<int> column_ids;         // projected schema indices, parallel to columns
  std::vector<ColumnVector> columns;
  size_t row_count = 0;
};
```

(First cut may drop nulls/validity to shrink scope — see Decisions.)

## The mediator interface

```cpp
class IBatchCursor {
 public:
  virtual ~IBatchCursor() = default;
  // Fill *out with the next batch (bounded row_count, e.g. 1024). Returns false
  // when the scan is exhausted.
  virtual bool Next(ColumnBatch *out) = 0;
};

class ITableSource {
 public:
  virtual ~ITableSource() = default;
  virtual const Schema &schema() const = 0;

  // Projection pushdown: `projection` are schema column indices to materialize.
  // A columnar source reads only those columns; a row source decodes only those
  // fields from each row blob.
  virtual std::unique_ptr<IBatchCursor> Scan(const std::vector<int> &projection) = 0;

  // --- optional capabilities (default: not supported) ---
  // Filter pushdown, statistics, ordering guarantees, etc. are added as separate
  // capability interfaces so the query layer can probe (dynamic_cast) and adapt,
  // exactly like the KV layer degrades gracefully. Deferred past the first cut.
};
```

## How the row/KV engine plugs in (RowTableSource)

A row/KV engine needs one new primitive: an **ordered scan** over its key space
(this is the previously-deferred iterator, now with a clear purpose). Add it to
the KV seam:

```cpp
// on IStorageEngine
virtual std::unique_ptr<IKvCursor> NewCursor() = 0;   // ordered (Slice key, Slice value)
```

`BPlusTreeEngine` implements it by walking the B+Tree leaf chain (leaves already
hold `NextPageId`) to get each `(EncodedKey, RID)`, then fetching the value bytes
from the `TupleStore`.

`RowTableSource` then composes:
- a `Schema`,
- a `RowCodec` (schema-aware: encodes a tuple -> row blob, decodes blob -> the
  projected fields), which lives in the **table/app layer**, not in the engine,
- the KV cursor.

Its `IBatchCursor::Next` pulls up to N rows from the KV cursor, decodes each row
blob through the `RowCodec` for the projected columns only, and appends into the
`ColumnVector`s of the batch. So a row engine becomes a valid `ITableSource`
purely by row→column materialization, reusing everything from Phases 0-4.

`RowCodec` (tuple <-> blob) formalizes the "EncodeRow/DecodeRow" idea: it is the
schema-aware application-layer codec, sitting above the schema-agnostic KV engine.

## How a native columnar engine plugs in

`ColumnarTableSource` implements `ITableSource` directly: it stores each column
in its own page chain / file, and `Scan(projection)` reads only the requested
column chains, emitting `ColumnBatch`es without ever touching the KV seam. Later
it can also implement filter-pushdown / statistics capabilities to skip blocks.
It does **not** reuse `IStorageEngine`; it is a sibling under `ITableSource`.

## Build phases

- **T0 — KV ordered cursor** (un-defer the iterator, now purposeful):
  `IKvCursor` (`Seek/Next/Valid/Key/Value`) + `IStorageEngine::NewCursor()`;
  `BPlusTreeEngine` implements it via leaf-chain + TupleStore. Test: full ordered
  scan returns all (key,value) in key order.
- **T1 — schema + batch types**: `Column/Schema`, `ColumnVector`, `ColumnBatch`.
- **T2 — RowCodec**: schema-aware tuple <-> blob (fixed + var-len columns).
- **T3 — ITableSource/IBatchCursor + RowTableSource**: row engine as a table;
  projection materializes only requested columns. Test: scan+project over a
  multi-column table backed by the B+Tree engine.
- **T4 — a trivial executor op** (scan → project → print/collect) to prove the
  query layer only sees `ITableSource`.
- **T5 (future) — ColumnarTableSource**: native columnar backend + projection
  (and later filter) pushdown; same `ITableSource`, query layer unchanged.

Phases T0-T4 are almost entirely **additive** — the KV engine, B+Tree, TupleStore
and catalog from Phases 0-4 stay as the row backend. Only `IStorageEngine` gains
`NewCursor()`.

## Decisions to settle before building

1. **Batch representation**: Arrow-lite column vectors (packed fixed-width +
   offsets/bytes for var-len, with a validity bitmap) — recommended, it is what a
   real columnar engine emits. Simpler alternative: row-of-variant `Datum`, but
   that throws away the columnar benefit. Recommendation: Arrow-lite, but drop
   the **validity bitmap (nulls)** in the first cut to shrink scope, add later.
2. **Projection now, filters later**: implement projection pushdown in T3; add
   filter pushdown / statistics as optional capability interfaces afterwards.
3. **RowCodec format**: length-prefixed fields is enough for a playground; column
   order follows the schema. Fixed-width fields inline; var-len as (len, bytes).
4. **Where the schema lives on disk**: extend the Phase 4 `MetaPage` (or a
   dedicated catalog page) to persist the table `Schema`, so a table can be
   reopened and scanned. (Today the meta only stores single key/value `Type`.)
```
