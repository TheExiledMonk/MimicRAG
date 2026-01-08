# MimicDB v0 Benchmark Plan: Postgres vs DuckDB

## Goal
Compare MimicDB sequential scan performance against PostgreSQL and DuckDB on simple filters.

## Dataset
- 10M rows
- Columns: int32, int32, float64
- Uniform distribution for filters

## Workloads
1) Equality filter on int32
2) Range filter on int32
3) Range filter + projection
4) Masked aggregation (SUM, COUNT)

## Metrics
- Rows/sec
- Bytes/sec
- Wall time
- Peak RSS

## Procedure
- Warm cache before timed runs
- Pin single thread
- Run 5 iterations, report median

## Notes
- Use sequential scans only; no indexes
- Keep storage format comparable (raw arrays where possible)
