# MimicDB vector database implementation TODO

## Exact-search foundation

- [x] Add a stable `vector_float32` field type without renumbering existing types.
- [x] Store each vector as packed IEEE-754 float32 values with a per-row byte length.
- [x] Validate non-empty, finite query vectors and consistent stored dimensions.
- [x] Persist and recover vector columns through sealed segment files (kept uncompressed for direct search).
- [x] Add scalar distance kernels and AVX2/FMA dot-product and cosine kernels.
- [x] Runtime-dispatch AVX2/FMA kernels with safe scalar fallback.
- [x] Implement parallel segment search with worker-local bounded top-k heaps.
- [x] Return stable dataset row IDs and distances.
- [x] Support null vectors and reject malformed vector payloads safely.
- [x] Add numeric metadata predicates before distance evaluation.

## API and protocol

- [x] Add an append-only vector-search opcode; keep all existing opcode values stable.
- [x] Mirror request/response packing in the C++ server and Python client.
- [x] Expose `vector_search` in both embedded C++ extensions and the high-level Python API.
- [x] Add capability authorization for `query.vector` using dataset scope.
- [ ] Document metrics, ordering, error behavior, and wire layouts.

## Verification and performance

- [x] Unit-test core metric calculation and top-k ordering.
- [ ] Test nulls, dimension mismatch, ties, `k=0`, and invalid metric IDs.
- [x] Test sealed segments, persistence/recovery, embedded calls, and network round trips.
- [x] Add a configurable exact-search benchmark covering row count, dimension, and top-k.
- [x] Report vectors/sec, effective bandwidth, and latency.
- [ ] Add configurable performance thresholds without hardware-specific defaults.

## Production ANN follow-up

- [ ] Define a versioned immutable per-segment index sidecar format.
- [ ] Add HNSW build-on-seal and background rebuild with checksums/atomic rename.
- [ ] Search sealed HNSW indexes and brute-force the active segment, then merge top-k.
- [ ] Add `ef_search`, `ef_construction`, and `M` configuration with safe limits.
- [ ] Measure recall@k against exact search and gate ANN releases on recall targets.
- [ ] Add tombstone/update handling, compaction, index observability, and repair tools.
- [ ] Evaluate scalar/product quantization only after representative memory benchmarks.
