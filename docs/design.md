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

## API-only joins (future)

Joins live only in the API layer. They compose existing primitive scans and
aggregations without adding new engine logic.

## API-only SQL surface (future)

SQL parsing is an API concern that maps SQL to primitive operations. The engine
remains SQL-agnostic.

## Networking (future)

Networking is a thin transport layer that maps 1:1 to existing core operations.
The server does not interpret or compose queries; it only dispatches the same
primitive calls that the in-process API uses.
Requests map directly to Dataset construction, AppendBatch, and aggregate scans.

### Service lifecycle (v0 draft)

Startup:

- Provide bind address/port via args or env (e.g., `PCDB_BIND=0.0.0.0:9000`).
- Start listener, accept loop, and dataset registry.
- Log version, port, and storage root.

Shutdown:

- Trap SIGINT/SIGTERM.
- Stop accepting new clients.
- Drain active client handlers to completion.
- Flush any pending segment writes if IO is enabled.

Config (minimal):

Config file `pcdb.conf` (or override with `PCDB_CONFIG`):

- `bind=127.0.0.1:9000`
- `storage_root=./data`
- `flush_on_shutdown=false`
- `log_level=info` (reserved)

### Housekeeping / remote sync (post‑networking)

Housekeeping runs only when the server is idle (no active clients). Hooks are
placeholders for future recovery/maintenance and remote sync once networking
expands beyond local use.

### Binary protocol framing (v0 draft)

All messages are little-endian.

Header (20 bytes):

- u32 magic = 0x50434442 ("PCDB")
- u16 version = 1
- u16 flags (bit 0 = response)
- u16 opcode
- u16 status (0 = ok, 1 = bad_request, 2 = not_found, 3 = internal_error, 4 = unsupported)
- u32 payload_size
- u32 request_id

Opcodes:

- 1: PING (request empty, response empty)
- 2: CREATE_DATASET
- 3: APPEND_BATCH
- 4: QUERY_AGG (full-scan aggregate over a field, no predicates yet)
- 5: HEALTH (request empty, response = dataset/segment/row counts)

Payloads:

CREATE_DATASET:

- u16 name_len
- bytes name
- u16 field_count
- repeat field_count:
  - u16 field_name_len
  - bytes field_name
  - u8 field_type (pcdb::FieldType enum)

APPEND_BATCH:

- u16 dataset_name_len
- bytes dataset_name
- u32 row_count
- u16 field_count
- repeat field_count:
  - u16 field_index
  - u8 field_type
  - u8 validity_mode (0 = all valid, 1 = bitmap)
  - u32 element_count (must equal row_count)
  - bytes data (count * sizeof(type))
  - bytes validity bitmap if validity_mode == 1 (ceil(count / 8))

QUERY_AGG:

- u16 dataset_name_len
- bytes dataset_name
- u16 field_index

QUERY_AGG response payload:

- u64 count
- f64 sum
- f64 min
- f64 max
- u8 has_value

HEALTH response payload:

- u16 dataset_count
- u64 segment_count
- u64 row_count
