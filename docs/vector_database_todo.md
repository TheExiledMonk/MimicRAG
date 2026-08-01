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

## Specialist IVF ANN

- [x] Build a compact in-memory coarse-quantizer over immutable sealed rows.
- [x] Store packed list offsets and row IDs without a general-purpose ANN dependency.
- [x] Route on a compact dimension sample, probe configurable lists, and exactly rerank candidates.
- [x] Intersect structured predicates before candidate vectors are loaded or scored.
- [x] Search the mutable active segment exactly and merge it with sealed-list results.
- [x] Publish immutable index snapshots so concurrent searches do not hold the build lock.
- [x] Expose opt-in IVF and probe count through embedded C++, Python, and the wire protocol.
- [x] Measure recall@k against exact search on random and clustered vector workloads.
- [x] Select high-variance routing dimensions instead of fixed coordinate spacing.
- [x] Prune impossible IVF lists with safe numeric predicate min/max bounds.
- [x] Add adaptive default probing with explicit probe overrides.
- [x] Add compact list-ordered int8 sketches and exact shortlist reranking for broad probes.
- [x] Reuse the persistent vector worker pool for query-time index work.
- [x] Parallelize full-dataset centroid assignment and expose per-stage timing counters.
- [x] Cache vector norms and fuse list-ordered SIMD scoring with worker-local top-k.
- [x] Maintain an append-sensitive micro-index for the mutable active segment.
- [x] Benchmark recall across a query suite instead of relying on a single query.
- [x] Reduce learned routing to 32 dimensions after a recall-gated projection sweep.
- [x] Scale broad-probe shortlist capacity by probe pressure, candidate volume, and routing confidence.
- [x] Detect poorly clustered cosine fields and route automatic IVF requests to exact CPU/Vulkan search.
- [x] Add opt-in vector-worker CPU affinity for latency-sensitive dedicated hosts.
- [ ] Persist a versioned per-segment IVF sidecar and load it during recovery.
- [ ] Rebuild changed sealed indexes in the background rather than on the first ANN query.
- [ ] Add representative-dataset recall calibration for automatic default probe selection.
- [ ] Add tombstone/update handling, compaction, index observability, and repair tools.
- [ ] Evaluate scalar/product quantization only after representative memory benchmarks.
- [ ] Revisit HNSW only for workloads whose recall/latency curve IVF cannot meet.
