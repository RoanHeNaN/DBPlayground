# Storage Engine Refactor: A Fully Type-Erased KV Engine

> Status: design locked. Implementation proceeds phase by phase.
> Companion reading: [`../template/TypeSystem.md`](../template/TypeSystem.md) —
> the *why* (compile-time vs runtime type, erasure at the engine boundary,
> `Column<T>` at the homogeneous granularity). This doc is the *how* for
> DBPlayground specifically.

## Goal

Let DBPlayground store **many key/value types** through **one** storage engine,
so that future engines and a future query engine can talk to storage through a
single, type-agnostic interface.

The chosen shape (see `TypeSystem.md` for the derivation):

- **Erase types at the engine boundary.** The public interface is `Slice` in,
  bytes out. The engine never knows what type the bytes represent.
- **Keep the type system above and beside the tree**, never inside it.
- **The B+Tree collapses to a single concrete type** — no templates over user
  types, no factory switch, no combinatorial explosion.

## Architecture

```
Caller (knows its own types at compile time, like a std::map user)
   │  encode_key<int64>(k) / encode_value<string>(v)  → bytes
   ▼
MiniKV        Insert(Slice, Slice) / Get(Slice) -> optional<bytes>     ← erased boundary
   │          init(path): read meta page → key/value Type (for validation/decoding,
   │                                                        NOT for comparison)
   ▼
BPlusTreeEngine : IStorageEngine    (non-templated)
   ├─ KeyEncoder<T>: order-preserving encode → fixed-width EncodedKey
   ├─ TupleStore  : variable-length value bytes  ⇄  RID
   └─ BPlusTree<EncodedKey, RID>    ← SINGLE instantiation, ordering = memcmp
   ▼
BufferPool / DiskManager  (unchanged)

Codec<T> / KeyEncoder<T>   ← templates live only down here (the doc's Column<T>)
```

## Key design decisions

1. **Both key and value are erased to bytes at the engine boundary.**
   The public API carries no static type. Callers, who know their own types at
   compile time, encode/decode with free helpers `encode_key<T>` /
   `encode_value<T>` / `decode_value<T>`.

2. **Key uses order-preserving encoding + `memcmp`.** Ints are stored
   big-endian with the sign bit flipped; unsigned big-endian; floats via the
   IEEE-754 order-preserving trick (flip sign bit if positive, flip all bits if
   negative). The result: **byte order == value order**, so the tree only ever
   does `memcmp` and needs **no comparator and no knowledge of the key type**.
   This is the single decision that lets the tree be fully erased.

3. **Value is opaque bytes stored in a `TupleStore`; the leaf holds a fixed
   `RID`.** This is the *RID indirection* variant. The B+Tree leaf keeps its
   existing **fixed-size** record `[EncodedKey | RID]`, so the page layout and
   the count-based split/merge logic stay essentially untouched. Variable-length
   complexity is isolated in one new module (`TupleStore`).

4. **Variable-length KEYS are deferred.** The `EncodedKey` is a **fixed-width**
   POD (initially 8 bytes, covering int32/int64/float/double/bool). String/blob
   KEYS require variable-length keys inside the tree (slotted pages) and are a
   later phase. String/blob **values** are fine today — they are opaque bytes in
   the `TupleStore`.

5. **RID indirection == "always overflow".** Every value lives in the
   `TupleStore`, even tiny ones, so every `Get` pays one extra fetch. This is
   the simple, uniform rule. A later optimization can switch to "small values
   inline, large values overflow" if the extra hop matters.

### Why this collapses the tree to one type

Because key and value are encoded to bytes *before* reaching the tree:

- key → fixed-width POD `EncodedKey { char b[KEY_LEN]; }`, whose `operator<`
  is `memcmp`;
- value → `RID` (fixed 8 bytes, already in `RID.h`).

So there is exactly **one** instantiation, `BPlusTree<EncodedKey, RID>`.
`IStorageEngine` / `MiniKV` are naturally non-templated: one
`unique_ptr<IStorageEngine>` handles any type combination. No factory switch,
no Cartesian product.

## Phased plan (dependency order)

### Phase 0 — Slice / Type / Codec foundation  ← current
- `Slice{const char*, size_t}` view + `memcmp`-based `compare`.
- `Type` enum `{Invalid, Int32, Int64, Float, Double, Bool, String, Blob}`.
- `Codec<T>`: `Encode`/`Decode` for values (raw bytes, no ordering).
- `KeyEncoder<T>`: **order-preserving** fixed-width encoding for keys.
- `IStorageEngine` (non-templated) interface header — declaration only.
- **Acceptance:** unit tests prove `sign(memcmp(encode(a), encode(b))) ==
  sign(a <=> b)` for every supported key type, including negatives, zero, and
  min/max boundaries.
- Fully decoupled from the tree; independently testable.
- (Iterator is intentionally **not** part of this refactor for now.)

### Phase 1 — TupleStore (heap for variable-length values)
- Page-organized heap over the existing `BufferPool`.
- `RID Insert(Slice value)` / `Slice Get(RID)` / `Delete(RID)` /
  `RID Update(RID, Slice)` (may relocate → returns possibly-new RID).
- **Acceptance:** round-trip insert/get/delete/update across page boundaries.

### Phase 2 — B+Tree adaptation
- Define `EncodedKey` (fixed-width POD, `operator<` = `memcmp`).
- Instantiate `BPlusTree<EncodedKey, RID>`; leaf/internal page layout unchanged.
- **Acceptance:** existing `BPlusTreeTest` passes with `EncodedKey`/`RID`.

### Phase 3 — BPlusTreeEngine + MiniKV decoupling
- `BPlusTreeEngine : IStorageEngine` composes KeyEncoder + TupleStore + tree.
- `MiniKV` holds `unique_ptr<IStorageEngine>`; API becomes `Slice`-based:
  `Insert(Slice,Slice)` / `Get(Slice)->optional<bytes>` / `Remove(Slice)` /
  `Range(Slice left)->vector<bytes>`.
- Free helpers `encode_key<T>` / `encode_value<T>` / `decode_value<T>`.

### Phase 4 — Meta / Catalog page
- Extend `HEADER_PAGE_ID`: store `root_page_id`, `key_type`, `value_type`,
  `encoding_version`.
- `create(path)` writes meta; `init(path)` reads meta and wires the right
  `Codec<T>` / `KeyEncoder<T>`.
- **Acceptance:** create → close → reopen recovers type info correctly.

### Phase 5 — Application-layer multi-column (demo)
- `EncodeRow` / `DecodeRow` example showing multiple columns serialized into one
  value blob, proving "multi-column = application-layer type system on top of a
  type-erased byte engine" (see `TypeSystem.md`).

### Deferred (explicitly out of scope for now)
- **Iterator** (`IIterator`) — not implemented in this refactor.
- **Variable-length keys** (string/blob keys) — needs slotted pages.
- **Inline-small / overflow-large value** optimization — currently always
  overflow via RID.

## Two settled cross-cutting points

1. **Runtime type validation lives in the encode/decode helpers.** Under a pure
   `Slice` interface, `MiniKV` cannot tell whether some bytes "are an int". The
   only place with type knowledge is `encode_value<T>` / `decode_value<T>`. So
   the engine stores the declared `Type` (from the meta page) and the helpers
   assert `T == declared type`. Without this, a pure `Slice` API has zero type
   safety. **Decision: do it.**

2. **The tree only `memcmp`s and never sees a type.** The cost is that *every*
   key type must have an order-preserving encoding. Ints are easy; floats need
   the IEEE-754 sign/negation trick. Accepted — this is what buys us a
   comparator-free, type-agnostic tree.

## Risks / watch-outs

- **Order-preserving encoding correctness** (negatives, zero, NaN, min/max) —
  the highest-value unit tests; get these right first (Phase 0 acceptance).
- **TupleStore update/delete**: a grown value may not fit in place → delete +
  reinsert + rewrite RID; free-space management needed.
- **Test ripple**: existing `int64/int32` tests must move through the codec.
