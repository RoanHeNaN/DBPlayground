# Scalar expression engine: a virtual-slot dataflow that unifies CSE, projection pruning, and index pushdown

> Status: **design settled, unimplemented.** Scope = the *scalar* expression
> subsystem (its planning AND its execution), which lives in the Execution layer
> **above** `ITableSource`. Relational operators (join/agg/sort/exchange) are NOT
> in scope — they host the scalar engine, they are not expressed by it.
> Companion: [`StorageAbstraction.md`](StorageAbstraction.md) (ObjectStore /
> FileFormat / Codec, below `ITableSource`); [`ColumnarTableSource.md`](ColumnarTableSource.md)
> (the `ITableSource` / `Chunk` seam this sits on).

## What we set out to unify

One abstraction, five problems:

1. **CSE** — a repeated subexpression computed once.
2. **Repeated column reads** — the same column read once. *(We concluded this is
   the SAME problem as CSE: a column read is just a leaf node; deduping leaves =
   reading once.)*
3. **Special-expression pushdown** — `search()` / vector-distance functions
   evaluated by a storage-layer index instead of row-by-row in the engine.
4. **Projection pruning** — columns no expression needs are never read. *(Also
   the same graph: pruning = deleting nodes unreachable from the required
   outputs; the dual of dedup, which merges equal nodes.)*
5. **Cross-plan-node result sharing** — a value computed in a lower operator
   reused by a higher one without recompute (cf. Apache Doris #61092).

## The core thesis (and what it is NOT)

The unifying primitive is **a virtual slot in the tuple/Chunk dataflow**: a
column position whose value is *defined by an expression*, and whose **production
site can be relocated** by planning (hoisted to the lowest node where its inputs
exist, or pushed down into the scan). The reusable/shareable thing is a **column
in the dataflow**, not a node in an engine-side execution graph.

This is deliberately **not** the ClickHouse `ActionsDAG` / Velox `ExprSet` model
as the top-level abstraction. Those are *per-operator, engine-side* execution
DAGs; their scope is one operator, so they structurally cannot express (5)
cross-operator sharing, and they do not express (3): both push down via a
*separate* structure translated out of the expression IR (ClickHouse
`KeyCondition` / `MergeTreeIndexCondition`; Velox `ScanSpec`), the expression IR
itself never runs in storage.

So the design is **two layers, both needed**:

```
  Cross-operator dataflow contract  ── the virtual-slot tuple model (Doris/Impala)
    solves (3),(5) and the GLOBAL part of (1),(2),(4): where each slot is
    produced, how it rides up, capability pushdown at the scan.
        │  each operator's slots are materialized by ↓
  Per-operator evaluation kernel   ── an ActionsDAG-like scalar DAG
    solves the LOCAL part of (1),(2),(4): within one operator, dedup shared
    nodes, compute each once into a position, prune unreachable inputs.
```

The ActionsDAG-like kernel gives us everything ClickHouse/Velox give *locally*;
the virtual-slot contract adds what they don't: cross-operator sharing and
index pushdown, **under one slot abstraction**.

## Why (1)(2)(4) are literally one problem

Model the whole scalar computation as a typed DAG whose **leaves are column
reads**. Then:

- **(1) CSE** = merge structurally-equal interior nodes (one node, many parents).
- **(2) repeated reads** = merge equal *leaf* nodes — the same operation on leaves.
- **(4) projection pruning** = keep only nodes reachable from the required
  outputs; unreached leaves are never read (this is ClickHouse
  `removeUnusedActions`).

Dedup (merge equal nodes) and prune (drop unreachable nodes) are the two graph
operations; that is the entirety of (1)(2)(4).

## Plan / state separation (why "per-segment clone" is not a real cost)

An expression is two layers that must be kept apart:

- **Immutable plan** — the compiled DAG (nodes, types, function bindings). Does
  not change per segment/batch. Compiled **once, shared**.
- **Mutable evaluation context** — per-evaluation scratch: the bound index
  iterator, intermediate columns, the selectivity/row set, sentinels. Naturally
  scoped to the evaluation unit; **a segment/batch is that unit**.

Keep them apart and per-segment isolation is *free* — each segment gets a fresh
context; the plan pointer is shared. This is exactly Velox (`ExprSet` immutable +
`EvalCtx` per-eval) and ClickHouse (`ActionsDAG` immutable + a per-call
execution context). Doris clones the whole expr per segment only because it
attaches segment-bound index state onto the (otherwise shareable) expr node; with
state in a per-segment context there is nothing to clone. **We adopt plan/state
separation, so "producer-context scope" is not a standing tax.**

## Two lifetime axes (the earlier confusion, resolved)

- **Axis A — materialization timing / data volume.** Early-materializing a cheap
  derived slot lets the fat input be dropped or never read. For `MATCH(content,
  'x')` where `content` is a 500 KB string and the result is one byte: producing
  the slot at the scan (via index) carries a 1-byte column up instead of a 500 KB
  string, and the string need not be read at all. **Here the virtual-slot / early
  path WINS on lifetime; per-operator recompute *maximizes* the cost by pinning
  the fat input alive end to end.** (Placement is otherwise cost-based: for a
  very selective downstream filter, late materialization of few rows can beat
  early materialization of all — so production-site placement is an optimization,
  but for large-input/small-output functions early wins decisively, and it makes
  (3) and (4) compound: the index answers the predicate, so the heavy column is
  neither read nor carried.)
- **Axis B — producer-context scope.** Dissolves under plan/state separation +
  static production (see above). It only reappears for *dynamic* production, and
  even then is handled by materializing within the producing operator (below).

## The virtual slot and the `ColumnNothing` sentinel

A slot has three states:

- **absent** — not in this Block's layout at all.
- **present-but-unmaterialized** — a typed placeholder (`ColumnNothing`): the
  position and type exist, the data does not yet.
- **present-real** — a materialized `Column`.

`ColumnNothing` exists to represent the middle state: "declared, but produced
elsewhere/later." Its job only exists once production can be *deferred or
relocated* — i.e. pushdown / capability-fallback (E2). In a pure static engine a
slot is either read (real) or not projected (absent); there is nothing for the
middle state to represent.

### Design decision: `ColumnNothing` is confined to the scan

**Rule: before a scan emits a Block, every `ColumnNothing` slot in it is
force-materialized. `ColumnNothing` therefore never crosses an operator
boundary.** What rides up the plan is always a real column.

Why confine it (these are the load-bearing reasons):

1. **Preserve a strong global invariant.** With confinement, *"a column present
   in a Block is real, materialized data"* holds at every operator boundary above
   the scan. Every operator and scalar function can use a slot without checking.
   Let `ColumnNothing` travel and the invariant weakens to *"a column might be a
   placeholder — check first,"* a nullable-everywhere tax where every read site is
   a forgotten-check bug.
2. **Don't infect generic columnar plumbing.** If placeholders travel, then
   filter / sort-permute / hash / serialize-for-exchange / spill must all handle a
   Nothing case. Confining it keeps Nothing inside the scan's column-production
   code — where virtual-column logic lives anyway — and out of the generic
   operator machinery.
3. **Remove the spill/exchange landmine.** A Nothing Block crossing a pipeline
   breaker (spill) or the network (shuffle) would arrive where its **producer
   context no longer exists** — the index iterators / segment state that could
   materialize it are gone → crash or stale data. Materializing at scan output
   guarantees the producer context is alive at materialization time.
4. **Keep the discipline local and auditable.** "Materialize before leaving the
   producer scope" collapses to a *single* enforcement point (scan output), not a
   distributed rule sprinkled before every breaker.
5. **The value, not the placeholder, is what's shared.** Cross-operator sharing
   (5) and the Axis-A win still hold, because the thing that rides up is the real
   (cheap) materialized column. Confinement costs the abstraction nothing here.

### Dynamic production without a traveling sentinel

Even capability-fallback ("use the index if present, else evaluate normally")
does **not** need a traveling sentinel: the producing operator (the scan) runs
whichever branch at runtime and **both branches emit a real column** at output.
The runtime choice is internal; the invariant holds. A *traveling* `ColumnNothing`
is required only for **cross-operator lazy deferral** (materialize at a later
consumer) — an aggressive optimization we do not need for E2, and which, if ever
adopted, is taken on consciously and scoped to the few operators involved.

## How pushdown (3) works here

`search()` / vector-distance become virtual slots whose production site the
planner pushes to the scan. At the scan:

- probe the source for an index capability (via `dynamic_cast` to an optional
  capability interface — *not* baked into the base `ITableSource`/`IFileFormat`
  seam; this is the deferred "filter/statistics pushdown capability" from
  `ColumnarTableSource.md`);
- if capable, evaluate via the index (e.g. bitmap → typed column); else evaluate
  the expression normally, right there at the scan;
- either way emit a real column at scan output.

This is the "produce-within-operator" rule from above, and it is why (3) needs no
traveling sentinel. Pushdown still requires a capability-negotiation boundary and
(for a real system) a restricted expression dialect the source understands — that
boundary is the honest, irreducible cost of crossing into storage (Trino's
`ConnectorExpression`, Doris's `IndexExecContext`+`fast_execute` are the
same shape).

## Cross-node sharing (5): cheap vs expensive

- **Cheap (in scope).** Relocate production **down** to the lowest common node
  (often the scan); the real column rides up the existing pipeline and any higher
  operator reuses it by referencing the slot — no spool, no lifetime problem.
  Pushing production down often *converts* a would-be cross-breaker share into an
  along-dataflow one.
- **Expensive (out of scope).** Two consumers separated by a pipeline breaker /
  in different fragments need the intermediate **materialized/spooled** with
  cost-based reuse-vs-recompute. The DAG can *express* it, but *realizing* it is a
  scheduling/materialization concern (Spark `ReuseExchange`), **not** part of the
  expression IR. We exclude it explicitly.

## Difference vs ClickHouse / Velox (the precise delta)

| | ClickHouse `ActionsDAG` / Velox `ExprSet` | This design |
|---|---|---|
| (1) CSE, (2) repeated reads, (4) pruning **within one operator** | ✅ (shared DAG node, `removeUnusedActions` / lazy vectors) | ✅ same, via the per-operator kernel |
| (5) cross-operator sharing | ❌ per-operator scope; ClickHouse recomputes across steps, Velox `ExprSet` is per-operator | ✅ the slot is a real column in the dataflow → any higher operator reuses it |
| (3) index pushdown via the **same** abstraction | ❌ separate structure translated out of the expr IR (`KeyCondition` / `ScanSpec`) | ✅ a virtual slot whose production is pushed to the scan; one slot abstraction covers it |

So the user's read is correct: **everything ClickHouse/Velox solve, this solves
(via the per-operator kernel); and on top, the virtual-slot dataflow contract
solves cross-operator sharing (5) and folds index-computed special expressions
(3) into the same slot abstraction.** The price of that extra reach:

- a **global slot / tuple-descriptor contract** — every operator agrees on slot
  ids and passes through slots it does not touch. ClickHouse/Velox stay simpler
  precisely by being per-operator; that simplicity is what they trade for not
  unifying (3) and (5).
- a **capability-negotiation boundary** at the scan for (3).

## Three orthogonal axes — keep them separate

The long analysis kept collapsing distinct concerns; the settled decomposition:

1. **CSE** — structural dedup on the DAG. (E1)
2. **Short-circuit / conditional evaluation** — *which rows* a node is computed
   for (`CASE`, `AND/OR`). Independent of CSE; every engine needs it or not on its
   own. A guarded common subexpression is **not** a CSE to hoist (DataFusion's
   `conditional_children` explicitly refuses). (E3, optional)
3. **Reuse lifetime of a materialized slot** — only bites with cross-scope reuse;
   dissolves under plan/state separation + scan-confined materialization. (E2)

Do not conflate them (an earlier `CASE`-based "CSE bug" example was a category
error: it was a short-circuit concern, not CSE, and afflicts all models equally).

The genuine interaction that *does* need row-set tracking: a **real** CSE that
meets short-circuit, e.g. `f(a)` in both `WHERE g(a) OR f(a)>5` and the SELECT —
the filter computes `f(a)` for one row set, the projection needs another. To
*reuse* across them you need a row-set-keyed slot (Velox `sharedSubexprResults_`
keyed by input vectors, tracking rows via `SelectivityVector`, extending by the
missing-rows delta); ClickHouse instead declines the cross-step reuse and
recomputes. This is the cost of *reuse + short-circuit*, not of CSE or the
sentinel.

## Phases

- **E1 — static scalar DAG.** Per-operator ActionsDAG-like kernel: CSE (dedup) +
  projection pruning (reachability) + eager, statically-placed production;
  cross-operator sharing of eagerly-materialized slots (the value rides up). No
  traveling sentinel, no `ColumnNothing`. Leaf reads call
  `ITableSource::Scan(referencedCols)` (I/O pruning). Strong Block invariant holds.
- **E2 — index / capability pushdown (committed).** Virtual columns whose
  production is pushed to the scan; `ColumnNothing` introduced **but scan-confined
  and force-materialized at scan output**; index capability probed via an optional
  interface on the source. Both fallback branches emit real columns.
- **E3 — optional.** Short-circuit / selectivity for conditional evaluation;
  row-set-keyed slot reuse; cross-operator lazy deferral (the only case that lets
  `ColumnNothing` travel — adopted consciously, scoped).
- **Explicitly out of the IR.** Cross-pipeline-breaker reuse (ReuseExchange-style
  materialization) — a separate, cost-based mechanism.

## Placement relative to existing code

Everything here is in the **Execution** layer, above `ITableSource`. Leaf reads
call `ITableSource::Scan(projection)`; the columnar reader keeps doing byte-range
projection (C3). E1 changes nothing below `ITableSource`. E2 adds an *optional*
index-capability interface probed (via `dynamic_cast`) on the source; the base
`ITableSource` / `IFileFormat` / `IStorage` seams stay unchanged.
