# MimicDB Python client reference

Import `MimicDBClient` from `client.mimicdb_client`. Construct it with `host`, `port`, `default_db`,
`identity_key_path`, and a strict known-hosts policy, then call `connect()` and close it after use.

Core operations include:

- `ping()` and `health()` for connectivity and state.
- `create_database(name)` and database listing/deletion methods.
- `create_dataset(name, fields, database=...)` with typed field definitions.
- Typed batch append methods for ingestion.
- `scan(...)` for bounded projection and predicates.
- `query_agg(...)` for server-side aggregation.
- Vector search methods for finite, dimension-matched float vectors.

Supported field types include `int32`, `int64`, `float64`, `bool`, `dict_int32`, `string`, `bytes`,
and `vector_float32`. Consult the checked-in client for the exact method signature matching the
deployed server version; the binary protocol is versioned and should not be reimplemented ad hoc.
