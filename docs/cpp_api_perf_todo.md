# C++ API Performance TODO

Focus: speed up C++ API bindings (`api_cpp`, `api/mimicapi/_mimicdb.cpp`) without changing core semantics.
Compression is server-only; the API layer must not decode or inspect compressed buffers.

## Hot paths
- [ ] Avoid per-row Python object creation when not needed (aggregate-only fast path) (targets: `api/mimicapi/_mimicdb.cpp`)
- [ ] Reuse per-call scratch buffers for mask + row-id collection (targets: `api_cpp/src/mimicapi_core.cpp`, `api/mimicapi/_mimicdb.cpp`)
- [ ] Batch row materialization into contiguous buffers before Python conversion (targets: `api/mimicapi/_mimicdb.cpp`)

## Predicate handling
- [x] Precompile predicate field indices + types once per call (targets: `api_cpp/src/mimicapi_core.cpp`, `api/mimicapi/_mimicdb.cpp`)
- [x] Add fast path for single equality predicate (skip full mask build) (targets: `api_cpp/src/mimicapi_core.cpp`, `api/mimicapi/_mimicdb.cpp`)

## Aggregation
- [x] Combine multiple aggregates into a single pass in the C++ core API (targets: `api_cpp/src/mimicapi_core.cpp`)
- [x] Expose a multi-aggregate API in the Python C++ binding to avoid repeated scans (targets: `api/mimicapi/_mimicapi_core.cpp`, `api/mimicapi/cpp_api.py`)

## Scan + projection
- [ ] Row-id first scan for C++ API, projection gather in a second pass (targets: `api_cpp/src/mimicapi_core.cpp`)
- [ ] Add limit/offset early exit in compressed scan loop (targets: `api_cpp/src/mimicapi_core.cpp`, `api/mimicapi/_mimicdb.cpp`)

## Threading
- [ ] Thread-local aggregates inside API scan/aggregate (server config only) (targets: `api_cpp/src/mimicapi_core.cpp`)
- [ ] Deterministic reduction in C++ API aggregates (targets: `api_cpp/src/mimicapi_core.cpp`)

## Diagnostics
- [ ] Expose rows scanned/pruned from C++ API without Python recompute (targets: `api_cpp/src/mimicapi_core.cpp`, `api/mimicapi/_mimicdb.cpp`)
- [ ] Add microbench for C++ API scan/aggregate vs Python baseline (targets: `benchmarks/bench_cpp_core.py`)
