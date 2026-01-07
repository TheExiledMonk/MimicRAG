# PCDB v0 Non-Negotiables

- The database stores facts, not programs
- The database executes no user logic
- The database exposes primitive operations only
- Expressiveness lives in the API layer, not the engine
- Performance comes from memory layout + vectorized scans, not planners

## Explicitly excluded in v0
- SQL
- Joins
- Stored procedures, triggers, UDFs
- Updates or deletes
- Query planners or cost models

## Dataset lock modes (future)

These are API-layer policies that gate which mutation operations are permitted. The core engine
remains append-only in v0.

- append-only: allows inserts only
- update-only: allows updates only
- full-crud: allows inserts, updates, deletes
