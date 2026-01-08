# MongoDB API (C++) Optimization TODO

Scope: C++ Mongo API layer only (`api_cpp/src/mimicapi_mongo.cpp` + `api_cpp/src/mimicapi_core.cpp`).
No server/engine logic changes beyond existing scan/aggregate primitives.

## 1) Incremental latest-doc cache
- [x] Track per-collection `last_seen_version`.
- [x] Maintain `_id -> latest doc` cache updated by `_version > last_seen_version` scans.
- [x] Use cache for `find/update/delete/aggregate` without full rescan.
- [x] Add invalidation on collection drop or schema reset.

## 2) Column pruning + projection-only decode
- [x] Build column list per query: `_id/_version/_deleted` + filter fields + projection fields.
- [x] Avoid decoding/allocating fields not requested.
- [x] Ensure computed projections still work with pruned columns.

## 3) Predicate pushdown into core scans
- [x] Translate `$match` filters into core predicates for `Scan(...)`.
- [x] Use pushdown in `find()` and `$match` stage of `aggregate()`.
- [x] Fallback to in-memory filter for unsupported operators only.

## 4) `_id` fast path
- [x] Special-case `_id == value` and `_id in [...]` for early short-circuit.
- [x] Scan minimal columns for `_id` lookups.
- [ ] Prefer ordered `_id` hits to avoid full map builds.
- [ ] (Option) API-maintained `_id` index stored in meta datasets for ordered/targeted lookups.

## 5) Aggregate pushdown (simple pipelines)
- [x] Detect pipelines: `$match` + `$group`/`$count`.
- [x] Route to core aggregate path without row materialization.
- [x] Keep Python fallback for complex stages.

## 6) Update/Delete optimization
- [x] Use `_id` fast path for updates/deletes when filter is `_id`-only.
- [x] Avoid building full `MongoDocument` for matched rows; extract only fields needed by update ops.
- [x] Batch update tombstones into a single append call.

## 7) Memory reuse in InsertMany
- [x] Reuse per-collection scratch buffers for typed batches.
- [x] Avoid repeated allocations for lengths/bytes buffers.
- [x] Add clear/reset strategy per call.

## 8) Validation & benchmarks
- [x] Add perf tests comparing full-scan vs incremental cache.
- [x] Track rows scanned/returned under Mongo C++.
- [x] Ensure correctness with mixed update/delete + find.
