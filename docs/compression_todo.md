# MimicDB Compression Implementation TODO (Server-Only)

Scope: Server-side compression only. No API/dialect/planner/GPU/distribution changes.

## 0. Design principles
- [ ] Compression is per-column, per sealed segment; active segments never compressed.
- [ ] Compression is scan-time friendly (SIMD + mask compatible).
- [ ] Compression choice is automatic and deterministic.
- [ ] No per-row/virtual dispatch in hot loops.

## 1. Column stats collection (seal time)
- [ ] Extend seal context to collect per-column stats (min, max, value count, null count).
- [ ] Add estimated cardinality (small hash/sample).
- [ ] Add monotonicity hint (best-effort).
- [ ] Persist stats in segment metadata (immutable).
- [ ] Ensure stats are O(n) at seal time only.

## 2. Codec abstraction (internal)
- [ ] Add ColumnCompressionKind enum: NONE, BIT_PACKED, DICTIONARY, FOR_DELTA, RLE (later), LZ4.
- [ ] Define compressed column contract:
  - decode_batch(mask, out_buffer)
  - scan_predicate(mask, predicate) where applicable
  - memory_bytes()
- [ ] Segment-level dispatch only (no per-row virtuals).

## 3. Compression selection heuristic
- [ ] Implement choose_compression(stats, column_type).
- [ ] Heuristic order:
  1) Dictionary if cardinality <= threshold
  2) Bit-pack if (max-min) fits in <= 1/2/4/8/16 bits
  3) Frame-of-reference delta for narrow numeric ranges
  4) RLE (optional, if long runs detected)
  5) LZ4 fallback
- [ ] All thresholds configured server-side (compile/config only).

## 4. Segment sealing pipeline
- [ ] Add compression phase at seal:
  - raw column -> compressed buffer
- [ ] Store compressed buffers + metadata alongside segment.
- [ ] Release raw buffers after successful compression.
- [ ] Keep uncompressed if ratio < 1.1x (configurable).
- [ ] Never compress partial segments; never recompress sealed segments.

## 5. Scan-time integration
- [ ] Branch once per segment: uncompressed vs compressed.
- [ ] Decode into SIMD-friendly buffers.
- [ ] Keep mask-first and mask-reuse behavior unchanged.
- [ ] Avoid full-column materialization.

## 6. Predicate pushdown on compressed data
- [ ] Dictionary: compare IDs directly.
- [ ] Bit-pack: compare packed values without full decode.
- [ ] FOR-delta: compare with base + delta.
- [ ] Fallback to decode+compare only when required.

## 7. Aggregation compatibility
- [ ] SUM/MIN/MAX/COUNT work on decoded batches.
- [ ] No double-decode across multiple aggregates.
- [ ] Reuse existing mask paths.

## 8. LZ4 fallback
- [ ] Integrate LZ4 block compression per column.
- [ ] Decode streaming blocks during scan.
- [ ] Avoid intermediate heap allocations.
- [ ] No predicate pushdown for LZ4.

## 9. Config + safeguards
- [ ] Config flags: enable/disable compression, per-codec toggles.
- [ ] Config: min segment size for compression.
- [ ] Sanity checks: compressed <= uncompressed, row counts match after decode.

## 10. Benchmark + validation
- [ ] Report compressed bytes vs raw bytes in benchmarks.
- [ ] Compare scan throughput before/after compression.
- [ ] Validate small datasets (no regressions).
- [ ] Validate large datasets (>= 1M rows).
- [ ] Validate multi-aggregate queries.
- [ ] Optional perf counters (bandwidth/branch misses).

## 11. Explicit out-of-scope
- [ ] API-level compression
- [ ] Query-specific codecs
- [ ] Adaptive per-query tuning
- [ ] GPU compression
- [ ] Cross-segment compression
- [ ] Transactional semantics
