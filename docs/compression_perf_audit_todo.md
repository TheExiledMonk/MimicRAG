## Compression/Decompression Perf Audit (Server-only)

### Decode + scan integration
- [x] Reuse decoded buffers per segment scan pass (avoid re-decode for multi-agg/query).
- [x] Decode only rows selected by mask for predicate-aware codecs.
- [x] Decode in fixed-size batches (cache-friendly chunking).
- [x] Skip decode entirely for columns not projected.

### Predicate pushdown + pruning
- [x] Apply segment min/max pruning before any codec work.
- [x] Compare dictionary IDs directly for equality predicates.
- [x] Bit-pack predicate evaluation without full decode (lookup/unpack path).
- [x] FOR-delta predicate evaluation without full decode.

### Buffer/allocator reuse
- [x] Reuse per-thread scratch buffers for decode output.
- [x] Reuse per-column aux buffers (lengths/offsets) where possible.
- [x] Avoid per-row allocations in compressed scan paths.

### Codec-specific fast paths
- [x] SIMD unpack for common bit widths (4/8/16).
- [x] SIMD delta add for FOR-delta decode.
- [x] Dictionary small-cardinality fast path (dense ID range).

### LZ4 fallback cost control
- [x] Decode LZ4 in streaming blocks only (no full-column materialization).
- [x] Keep LZ4 decode output in thread-local buffers.
- [x] Avoid random access to LZ4 columns in scan loops.

### Validation/metrics
- [x] Measure decode bytes/sec vs raw scan bytes/sec.
- [x] Add counters for “compressed columns touched” vs “skipped by pruning”.
- [x] Track time spent in decode vs predicate/aggregate.
