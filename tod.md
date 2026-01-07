# PCDB v0 TODO (Milestones)

## M0: Scaffolding and guardrails
Dependencies: none
- [x] Create repo layout (targets: `engine/`, `api/`, `tests/`, `benchmarks/`, `docs/`)
- [x] Add C++20 build skeleton (targets: `CMakeLists.txt`, `engine/CMakeLists.txt`)
- [x] Add Python package skeleton (targets: `api/pyproject.toml`, `api/pcdb/__init__.py`)
- [x] Add coding standards notes (targets: `docs/standards.md`)
- [x] Document non-negotiables (targets: `docs/design.md`)

## M1: Data model and storage
Dependencies: M0
- [x] Define field types enum/ids (targets: `engine/include/pcdb/types.h`)
- [x] Define validity bitmap (targets: `engine/include/pcdb/bitmap.h`, `engine/src/bitmap.cpp`)
- [x] Define FieldVector (append-only, immutable) (targets: `engine/include/pcdb/field_vector.h`, `engine/src/field_vector.cpp`)
- [x] Define Dataset metadata (targets: `engine/include/pcdb/dataset.h`, `engine/src/dataset.cpp`)
- [x] Enforce equal lengths on append (targets: `engine/src/dataset.cpp`)
- [x] Define segment format (targets: `engine/include/pcdb/segment.h`, `engine/src/segment.cpp`)
- [x] Choose segment size defaults (targets: `engine/include/pcdb/config.h`)
- [x] Implement append-only segment writer (targets: `engine/src/segment_io.cpp`)
- [x] Implement memory-mapped reader (targets: `engine/src/segment_io.cpp`)

## M2: Execution model and hot loop
Dependencies: M1
- [x] Define batch struct and sizing (targets: `engine/include/pcdb/batch.h`, `engine/src/batch.cpp`)
- [x] Implement predicate mask (targets: `engine/include/pcdb/mask.h`, `engine/src/mask.cpp`)
- [x] Implement mask composition AND/OR (targets: `engine/src/mask.cpp`)
- [x] Implement hot loop kernel over SoA (targets: `engine/include/pcdb/scan.h`, `engine/src/scan.cpp`)
- [x] Add SIMD kernel dispatch (targets: `engine/src/simd_dispatch.cpp`)
- [x] Ensure no RTTI/virtual in scan paths (targets: `engine/src/scan.cpp`)

## M3: Operators, API, and validation
Dependencies: M2
- [x] Filters: equality/inequality/range (targets: `engine/include/pcdb/predicate.h`, `engine/src/predicate.cpp`)
- [x] Projection: select subset of fields (targets: `engine/include/pcdb/projection.h`, `engine/src/projection.cpp`)
- [x] Aggregations: COUNT/SUM/MIN/MAX (targets: `engine/include/pcdb/aggregate.h`, `engine/src/aggregate.cpp`)
- [x] Ensure aggregations are masked, single-pass (targets: `engine/src/aggregate.cpp`)
- [x] Python Dataset API (targets: `api/pcdb/dataset.py`)
- [x] Python filter/aggregate API (targets: `api/pcdb/query.py`)
- [x] Ensure API is pure wrapper (targets: `api/pcdb/__init__.py`)
- [x] Unit tests for masks (targets: `tests/test_mask.cpp`)
- [x] Deterministic result tests (targets: `tests/test_queries.cpp`)
- [x] Performance stability checks (targets: `benchmarks/bench_scan.cpp`)
- [x] Metrics reporting (targets: `engine/include/pcdb/metrics.h`, `engine/src/metrics.cpp`)
- [x] Bench vs Postgres/DuckDB (targets: `benchmarks/bench_vs_pg_duckdb.md`)

## Global constraints (applies to all milestones)
- [x] No SQL support (targets: `docs/design.md`)
- [x] No joins/UDFs/triggers/stored procedures (targets: `docs/design.md`)
- [x] No updates/deletes/planners/cost models (targets: `docs/design.md`)
- [x] No per-row allocations or adaptive execution (targets: `docs/standards.md`)

## Doc gap breakdown (next implementation segments)
### Segment A: SoA storage + dataset append values
- [x] Add typed buffers to FieldVector (targets: `engine/include/pcdb/field_vector.h`, `engine/src/field_vector.cpp`)
- [x] Add append APIs per type on FieldVector (targets: `engine/include/pcdb/field_vector.h`, `engine/src/field_vector.cpp`)
- [x] Update Dataset::Append to accept values and write to all fields (targets: `engine/include/pcdb/dataset.h`, `engine/src/dataset.cpp`)
- [x] Add basic tests for typed append and null validity (targets: `tests/test_field_vector.cpp`)

### Segment B: Segment data layout + IO
- [x] Define on-disk segment layout for column buffers (targets: `engine/include/pcdb/segment_io.h`, `engine/src/segment_io.cpp`)
- [x] Write header + column buffers (append-only) (targets: `engine/src/segment_io.cpp`)
- [x] Map full segment data for reads (targets: `engine/src/segment_io.cpp`)
- [x] Add segment IO round-trip test with data (targets: `tests/test_segment_io.cpp`)

### Segment C: Batch processing and mask generation
- [x] Add batch iterator over segments (targets: `engine/include/pcdb/batch.h`, `engine/src/batch.cpp`)
- [x] Add predicate-to-mask generation helpers (targets: `engine/include/pcdb/mask.h`, `engine/src/mask.cpp`)
- [x] Add tests for mask generation (targets: `tests/test_mask.cpp`)

### Segment D: Hot loop variants + SIMD hooks
- [x] Add masked scan loop (targets: `engine/include/pcdb/scan.h`, `engine/src/scan.cpp`)
- [x] Add SIMD dispatch stub per type (targets: `engine/src/simd_dispatch.cpp`)
- [x] Add scan loop tests (targets: `tests/test_scan.cpp`)

### Segment E: Aggregation plumbing
- [x] Wire aggregations to FieldVector buffers (targets: `engine/src/aggregate.cpp`)
- [x] Add masked aggregation tests on dataset data (targets: `tests/test_queries.cpp`)

### Segment F: Metrics + perf harness
- [x] Add metrics counters to scans (targets: `engine/include/pcdb/metrics.h`, `engine/src/metrics.cpp`)
- [x] Extend `bench_scan.cpp` to report rows/sec and bytes/sec (targets: `benchmarks/bench_scan.cpp`)

## Core optimization roadmap (Tier 0)
### 1) Batched append (SoA input)
- [x] Add Python `append_batch` API (targets: `api/pcdb/dataset.py`)
- [x] Add C++ batch append entrypoint (targets: `engine/include/pcdb/dataset.h`, `engine/src/dataset.cpp`)
- [x] Pre-reserve field vectors for batch size (targets: `engine/src/field_vector.cpp`)
- [x] Bulk copy column buffers (targets: `engine/src/field_vector.cpp`)
- [x] Bulk validity bitmap generation (targets: `engine/src/bitmap.cpp`)
- [x] Add batch append tests (targets: `tests/test_python_api.py`, `tests/test_field_vector.cpp`)

### 2) Segment-based storage (hard batch boundary)
- [x] Enforce fixed segment size (targets: `engine/include/pcdb/segment.h`, `engine/src/segment.cpp`)
- [x] Seal segments after fill (targets: `engine/src/segment.cpp`)
- [x] Track per-segment metadata (min/max/nulls) (targets: `engine/include/pcdb/segment.h`, `engine/src/segment.cpp`)
- [x] Store metadata alongside segments (targets: `engine/include/pcdb/segment_io.h`, `engine/src/segment_io.cpp`)
- [x] Add segment metadata tests (targets: `tests/test_segment_io.cpp`)

### 3) Branchless predicate evaluation
- [x] Add branchless compare helpers (targets: `engine/include/pcdb/predicate.h`, `engine/src/predicate.cpp`)
- [x] Add mask generation without branches (targets: `engine/src/mask.cpp`)
- [x] Add tests for branchless predicates (targets: `tests/test_mask.cpp`)

## Core performance roadmap (Tier 1)
### 4) SIMD predicate + aggregate kernels
- [x] Add SIMD predicate kernels (targets: `engine/src/simd_predicate.cpp`)
- [x] Add SIMD aggregate kernels (targets: `engine/src/simd_aggregate.cpp`)
- [x] Add runtime CPU feature detection (targets: `engine/src/simd_dispatch.cpp`)
- [x] Add scalar fallback coverage tests (targets: `tests/test_simd.cpp`)

### 5) Mask-first execution model
- [x] Add explicit mask build pass (targets: `engine/src/mask.cpp`, `engine/src/scan.cpp`)
- [x] Add mask reuse in aggregates (targets: `engine/src/aggregate.cpp`)
- [x] Add tests for mask reuse semantics (targets: `tests/test_queries.cpp`)

### 6) Validity bitmap specialization
- [x] Add fast path for non-null columns (targets: `engine/src/aggregate.cpp`, `engine/src/scan.cpp`)
- [x] Track null_count per segment (targets: `engine/include/pcdb/segment.h`, `engine/src/segment.cpp`)
- [x] Add nullable vs non-nullable tests (targets: `tests/test_field_vector.cpp`)

## Scan reduction roadmap (Tier 2)
### 7) Segment pruning (min/max skip)
- [x] Track per-segment min/max (targets: `engine/include/pcdb/segment.h`, `engine/src/segment.cpp`)
- [x] Compare predicates vs segment min/max (targets: `engine/src/predicate.cpp`, `engine/src/scan.cpp`)
- [x] Add pruning tests (targets: `tests/test_segment_io.cpp`)

### 8) Dictionary encoding for low-cardinality fields
- [x] Add dictionary-encoded field type (targets: `engine/include/pcdb/types.h`, `engine/src/field_vector.cpp`)
- [x] Add dictionary builder + lookup (targets: `engine/src/dictionary.cpp`)
- [x] Add encoding tests (targets: `tests/test_field_vector.cpp`)

## Output efficiency roadmap (Tier 3)
### 9) Row-id first, values later
- [x] Add row-id output buffer (targets: `engine/include/pcdb/scan.h`, `engine/src/scan.cpp`)
- [x] Add projection gather pass (targets: `engine/src/projection.cpp`)
- [x] Add row-id tests (targets: `tests/test_scan.cpp`)

### 10) Vectorized output compaction
- [x] Add SIMD compress-store for masks (targets: `engine/src/simd_output.cpp`)
- [x] Add prefix-sum compaction (targets: `engine/src/output_compaction.cpp`)
- [x] Add compaction tests (targets: `tests/test_scan.cpp`)

## Parallelism roadmap (Tier 4)
### 11) Segment-level parallel scans
- [x] Add per-segment scan scheduling (targets: `engine/src/scan.cpp`)
- [x] Add thread-local aggregates and reduce (targets: `engine/src/aggregate.cpp`)
- [x] Add parallel scan tests (targets: `tests/test_scan.cpp`)

## Explicitly not adding (yet)
- [x] Joins (targets: `docs/design.md`)
- [x] Cost-based planner (targets: `docs/design.md`)
- [x] Adaptive execution (targets: `docs/design.md`)
- [x] Secondary indexes (targets: `docs/design.md`)
- [x] Transactions beyond append (targets: `docs/design.md`)
- [x] SQL frontend (targets: `docs/design.md`)
- [x] Stored logic (targets: `docs/design.md`)

## Golden rule
- [x] Every optimization must reduce rows scanned or bytes touched per row (targets: `docs/standards.md`)

## Post‑v0 optimization phase (v0.1–v0.3)
### Pruning diagnostics
- [x] Add segment scan/prune counters (targets: `engine/include/pcdb/metrics.h`, `engine/src/metrics.cpp`)
- [x] Expose pruning stats via Python API debug flag (targets: `api/pcdb/query.py`, `api/pcdb/dataset.py`)
- [x] Add pruning diagnostics tests (targets: `tests/test_python_api.py`)

### Aggregate kernel specialization
- [x] Add specialized aggregate kernels (count/sum/minmax/mixed) (targets: `engine/src/aggregate.cpp`, `engine/src/simd_aggregate.cpp`)
- [x] Add tests for aggregate specialization equivalence (targets: `tests/test_queries.cpp`)

### Mask handling optimizations
- [x] Add mask elision path for single‑consumer aggregates (targets: `engine/src/aggregate.cpp`, `engine/src/scan.cpp`)
- [x] Add bit‑packed mask representation for sparse matches (targets: `engine/include/pcdb/mask.h`, `engine/src/mask.cpp`)
- [x] Add mask compression tests (targets: `tests/test_mask.cpp`)

### Segment pruning enhancements
- [x] Evaluate predicates against segment metadata before scan (targets: `engine/src/scan.cpp`)
- [x] Add pruning behavior tests (targets: `tests/test_segment_io.cpp`)

### Validation and measurement
- [x] Record segments_total/segments_scanned/segments_pruned (targets: `engine/include/pcdb/metrics.h`, `engine/src/metrics.cpp`)
- [x] Add bytes/sec estimate in benchmarks (targets: `benchmarks/bench_scan.cpp`)

## Networking (future)
- [x] Define binary protocol request/response framing (targets: `docs/design.md`)
- [x] Implement server transport and request dispatcher (targets: `server/pcdb_server.cpp`)
- [x] Add dataset selection and query routing (targets: `server/pcdb_server.cpp`)
- [x] Stream query results back to client (targets: `server/pcdb_server.cpp`)
- [x] Add minimal client for round‑trip tests (targets: `client/pcdb_client.py`, `tests/test_network.py`)
- [x] Ensure server maps 1:1 to core operations (targets: `docs/design.md`)
- [x] Add CMake target for server binary (targets: `server/CMakeLists.txt`, `CMakeLists.txt`)

## Server lifecycle & recovery (future)
- [x] Add service lifecycle docs (startup/shutdown/config) (targets: `docs/design.md`)
- [x] Add graceful shutdown handling (signals, active client drain) (targets: `server/pcdb_server.cpp`)
- [x] Add config file support for bind/storage/flush settings (targets: `server/pcdb_server.cpp`, `docs/design.md`)
- [x] Add flush-on-shutdown option for active segments (targets: `server/pcdb_server.cpp`, `engine/src/dataset.cpp`)
- [x] Add dataset recovery from disk segments (targets: `server/pcdb_server.cpp`, `engine/src/segment_io.cpp`)
- [x] Add server state health/metrics endpoint or request (targets: `server/pcdb_server.cpp`)
- [x] Add restart safety test harness (targets: `tests/test_network.py`)

## Server housekeeping (future)
- [x] Add idle‑only housekeeping loop (targets: `server/pcdb_server.cpp`)
- [x] Add recovery/maintenance scheduling hooks (targets: `server/pcdb_server.cpp`)
- [x] Add remote sync hook placeholders (post‑networking) (targets: `docs/design.md`)

## Post‑networking (future, low priority)
- [x] Add idle‑only housekeeping thread for recovery/maintenance and remote sync (targets: `docs/design.md`)

## API‑layer extensions (post‑v0)
- [x] API‑only joins (targets: `api/pcdb/query.py`, `docs/design.md`)
- [x] API‑only SQL surface (targets: `api/pcdb/sql.py`, `docs/design.md`)

## Locking / mutation modes (future)
- [x] Define dataset lock modes: append‑only, update‑only, full CRUD (targets: `docs/design.md`)
- [x] Add API enforcement for lock modes (targets: `api/pcdb/dataset.py`)
