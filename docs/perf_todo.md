# MimicDB Performance Roadmap TODO

Goal: incremental, measurable speed improvements without violating the hot-loop rules.

## Hot loop + SIMD
- [ ] Vectorized inner loops for scan predicates (targets: `engine/src/simd_predicate.cpp`, `engine/src/scan.cpp`)
- [ ] Vectorized aggregate kernels (targets: `engine/src/simd_aggregate.cpp`)
- [x] SIMD output compaction (targets: `engine/src/simd_output.cpp`)
- [x] Runtime CPU feature detection + dispatch (targets: `engine/src/simd_dispatch.cpp`)

## Branchless execution
- [x] Branchless predicate evaluation helpers (targets: `engine/src/predicate.cpp`, `engine/src/mask.cpp`)
- [x] Mask-first execution path (targets: `engine/src/mask.cpp`, `engine/src/scan.cpp`)
- [x] Mask reuse in aggregates (targets: `engine/src/aggregate.cpp`)

## Multi-threading
- [ ] Segment-level parallel scans (targets: `engine/src/scan.cpp`)
- [ ] Thread-local aggregates + reduction (targets: `engine/src/aggregate.cpp`)
- [ ] Deterministic reduction ordering (targets: `engine/src/aggregate.cpp`)

## Reduction + output strategies
- [ ] Row-id first, projection later (targets: `engine/src/scan.cpp`, `engine/src/projection.cpp`)
- [ ] Prefix-sum output compaction (targets: `engine/src/output_compaction.cpp`)
- [ ] Sparse mask bit-packing (targets: `engine/src/mask.cpp`)

## Build + compiler tuning
- [ ] Enable LTO (targets: `CMakeLists.txt`)
- [ ] Tune `-march`/`-mtune` flags for host builds (targets: `CMakeLists.txt`)
- [ ] PGO harness + profile usage (targets: `benchmarks/bench_scan.cpp`, `CMakeLists.txt`)

## Measurement
- [ ] Standardize perf metrics output (rows/sec, bytes/sec, cache/branch misses) (targets: `engine/src/metrics.cpp`, `benchmarks/bench_scan.cpp`)
- [ ] Add perf regression thresholds (targets: `benchmarks/`)
