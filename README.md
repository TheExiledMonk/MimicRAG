# PCDB (Primitive Columnar Database)

PCDB is a minimal, high‑performance columnar engine focused on a single hot scan loop. It keeps data storage/query physics separate from higher‑level logic. v0 is intentionally narrow: append‑only, columnar storage, batch scanning, simple filters, projections, and masked aggregations.

## What it is

- A primitive columnar engine with SoA storage and a predictable scan loop.
- A strict separation between engine physics and API logic.
- An in‑process, single‑machine prototype (no network, no SQL).

## Strengths in v0

- **Predictable scan performance**: the core loop is explicit and simple.
- **Columnar layout**: contiguous, typed buffers are cache‑friendly.
- **Batch ingest**: `append_batch` supports SoA input for fast ingestion.
- **Segmented storage**: fixed‑size segments with min/max/null stats enable pruning.
- **Mask‑based execution**: predicates build masks, aggregations consume them.
- **Branchless predicate helpers**: groundwork for vectorization.
- **C++ core + Python API**: a thin binding (no Python logic in the engine).

## Weaknesses / limitations in v0

- **No SQL, joins, transactions, updates, deletes** — by design.
- **No cost model or planner**; execution is fixed and deterministic.
- **Single process, single machine** only.
- **Minimal SIMD**: stubs exist, but full vectorized kernels are not in place yet.
- **No networked API** or distributed execution.
- **Segment pruning is basic**: metadata is present, but advanced pruning is still in progress.

## Current capabilities

- Typed fields: `int32`, `int64`, `float64`, `bool` (plus `dict_int32` for low‑cardinality).
- Append‑only datasets with validity bitmaps.
- Batch scanning with predicate masks.
- Aggregations: `count`, `sum`, `min`, `max` (masked, single‑pass).
- Segment metadata: per‑field min/max/null counts.
- Python bindings (CPython C‑API), used for benchmarks.

## Quick start

### Build the Python extension (Python 3.12)

```
python3.12 -m venv --system-site-packages .venv
.venv/bin/python api/setup.py build_ext --inplace
```

### Run the Python benchmark (C++ backend)

```
.venv/bin/python benchmarks/bench_python_api.py \
  --rows 1000000 --batch-size 200000 --backend cpp --append-mode batch --debug
```

## Design rules (v0)

- The engine **does not execute user logic**.
- No loops or callbacks supplied by users inside the scan loop.
- No joins, planners, or adaptive execution.
- Append‑only, immutable segments once sealed.

## Planned additions (post‑v0)

### Tier 1 – Core performance
- SIMD predicate and aggregation kernels with runtime dispatch.
- Mask‑first execution with reuse across aggregates.
- Validity‑aware fast paths for non‑null segments.

### Tier 2 – Scan reduction
- Stronger segment pruning using min/max metadata.
- Dictionary encoding for low‑cardinality fields (already partially implemented).

### Tier 3 – Output efficiency
- Row‑id first execution and projection gathering.
- Vectorized output compaction.

### Tier 4 – Parallelism
- Segment‑level parallel scans with thread‑local aggregates and deterministic reduction.

### API and lifecycle controls
- Dataset lock modes (append‑only, update‑only, full CRUD) enforced in the API layer.

## Explicitly not adding (v0)

- SQL or joins in the core engine (these belong in the API layer only).
- Query planners or cost models.
- Secondary indexes (pruning only may be added later).
- Transactions beyond append atomicity.

## Philosophy

Every optimization must reduce **rows scanned** or **bytes touched per row**. If it does neither, it doesn’t belong in the core.
