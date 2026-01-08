# MimicDB Compression Implementation TODO (Server-Only)

Scope: Server-side compression only. No API/dialect/planner/GPU/distribution changes.

## 0. Design principles
- [x] Compression is per-column, per sealed segment; active segments never compressed.
- [x] Compression is scan-time friendly (SIMD + mask compatible).
- [x] Compression choice is automatic and deterministic.
- [x] No per-row/virtual dispatch in hot loops.

## 1. Column stats collection (seal time)
- [x] Extend seal context to collect per-column stats (min, max, value count, null count).
- [x] Add estimated cardinality (small hash/sample).
- [x] Add monotonicity hint (best-effort).
- [x] Persist stats in segment metadata (immutable).
- [x] Ensure stats are O(n) at seal time only.

## 2. Codec abstraction (internal)
- [x] Add ColumnCompressionKind enum: NONE, BIT_PACKED, DICTIONARY, FOR_DELTA, RLE (later), LZ4.
- [x] Define compressed column contract:
  - decode_batch(mask, out_buffer)
  - scan_predicate(mask, predicate) where applicable
  - memory_bytes()
- [x] Segment-level dispatch only (no per-row virtuals).

## 3. Compression selection heuristic
- [x] Implement choose_compression(stats, column_type).
- [x] Heuristic order:
  1) Dictionary if cardinality <= threshold
  2) Bit-pack if (max-min) fits in <= 1/2/4/8/16 bits
  3) Frame-of-reference delta for narrow numeric ranges
  4) RLE (optional, if long runs detected)
  5) LZ4 fallback
- [x] All thresholds configured server-side (compile/config only).

## 4. Segment sealing pipeline
- [x] Add compression phase at seal:
  - raw column -> compressed buffer
- [x] Store compressed buffers + metadata alongside segment.
- [x] Release raw buffers after successful compression.
- [x] Keep uncompressed if ratio < 1.1x (configurable).
- [x] Never compress partial segments; never recompress sealed segments.

## 5. Scan-time integration
- [x] Branch once per segment: uncompressed vs compressed.
- [x] Decode into SIMD-friendly buffers.
- [x] Keep mask-first and mask-reuse behavior unchanged.
- [x] Avoid full-column materialization.

## 6. Predicate pushdown on compressed data
- [x] Dictionary: compare IDs directly.
- [x] Bit-pack: compare packed values without full decode.
- [x] FOR-delta: compare with base + delta.
- [x] Fallback to decode+compare only when required.

## 7. Aggregation compatibility
- [x] SUM/MIN/MAX/COUNT work on decoded batches.
- [x] No double-decode across multiple aggregates.
- [x] Reuse existing mask paths.

## 8. LZ4 fallback
- [x] Integrate LZ4 block compression per column.
- [x] Decode streaming blocks during scan.
- [x] Avoid intermediate heap allocations.
- [x] No predicate pushdown for LZ4.

## 9. Config + safeguards
- [x] Config flags: enable/disable compression, per-codec toggles.
- [x] Config: min segment size for compression.
- [x] Sanity checks: compressed <= uncompressed, row counts match after decode.

## 10. Benchmark + validation
- [x] Report compressed bytes vs raw bytes in benchmarks.
- [x] Compare scan throughput before/after compression.
- [x] Validate small datasets (no regressions).
- [x] Validate large datasets (>= 1M rows).
- [x] Validate multi-aggregate queries.
- [x] Optional perf counters (bandwidth/branch misses).

## 11. Explicit out-of-scope
- [ ] API-level compression
- [ ] Query-specific codecs
- [ ] Adaptive per-query tuning
- [ ] GPU compression
- [ ] Cross-segment compression
- [ ] Transactional semantics
