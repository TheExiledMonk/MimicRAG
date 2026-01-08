# MimicAPI C++ Core Completion TODO

## Predicates & Filtering
- [x] Support string equality/inequality predicates
- [x] Support bytes equality/inequality predicates
- [x] Add null-aware predicates (IS NULL / IS NOT NULL)
- [x] Add type mismatch guards (fail fast)
- [x] Add predicate coverage tests

## Scan & Projection
- [x] Ensure offset/limit correctness for all field types
- [x] Support projection by column name list (including empty = all)
- [x] Add scan tests with mixed types (int/float/bool/string/bytes)

## Aggregation
- [x] Explicitly reject non-numeric aggregates in API (string/bytes)
- [x] Add aggregation tests with predicates (numeric only)
- [x] Add rows_scanned tracking tests

## Batching & Append
- [x] Validate append_batch count consistency across fields
- [x] Validate append_batch field existence and types
- [x] Add append_batch tests for varlen fields

## Data Access
- [x] Optimize varlen reads (avoid O(n) offset scan per row)
- [x] Add caching for varlen offsets per field vector

## Error Handling
- [x] Standardize error reporting from C++ core to Python wrapper
- [x] Add error tests (unknown dataset, invalid predicate, type mismatch)

## Bench & Integration
- [x] Add C++ core benchmark harness
- [x] Integrate C++ core into bench_suite (flag or variant)
