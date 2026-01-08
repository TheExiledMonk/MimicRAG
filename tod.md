# API Layer TODO (Smart Layer)

Purpose:
The API layer is responsible for all intelligence: coordination, fan-out,
replication, retries, composition, and user-facing semantics.
The database server remains a pure execution + storage engine.

## 1. API Core Structure
- [x] Define ApiClient core abstraction
- [x] Manage one or more server connections (local or network)
- [x] Own policy decisions (stateless for query semantics)
- [x] Build connection registry (host/port or local handle)
- [x] Track connection health per server
- [x] Track per-server latency

## 2. Transport Abstraction
- [x] Define unified transport interface
- [x] Implement LocalTransport
- [x] Implement NetworkTransport
- [x] Ensure identical request/response shape for both
- [x] Implement transport selection logic in API

## 3. Write Path (Replication & Fan-out)
- [x] Implement parallel fan-out writes for append/append_batch
- [x] Generate batch_id for every append_batch
- [x] Include batch_id in all write requests
- [x] Allow safe retries/replays using idempotent batch_id
- [x] Implement write policy (default quorum: floor(N/2)+1)
- [x] Accept late acknowledgements (non-blocking)
- [x] Fail fast when quorum cannot be reached
- [x] Capture per-server success/failure results

## 4. Read Path (Query Coordination)
- [x] Route reads to fastest healthy server by default
- [x] Track latency per server for routing
- [x] Add optional read verification mode
- [x] Sample secondary server and compare metadata
- [x] Implement consistency modes (API-only): any, quorum
- [x] Keep servers unaware of consistency modes

## 5. Failure Handling & Health Management
- [x] Track session-level server health
- [x] Mark server unhealthy on failure
- [x] Exclude unhealthy servers from routing
- [x] Add blacklist TTL and retry after expiry
- [x] Make TTL configurable
- [x] Implement bounded retries (no storms)
- [x] Respect idempotency on retries

## 6. Recovery & Replay (Client-Side)
- [x] Implement append replay log (API-managed)
- [x] Record dataset, batch_id, payload reference
- [x] Replay missing batches when a server recovers
- [x] Keep replay best-effort and non-blocking
- [x] Use in-memory log for v0
- [x] Add optional disk-backed log later

## 7. API-Level Composition (Smart Stuff Lives Here)
- [x] Add query composition helpers
- [x] Fan-out queries and merge partial results
- [x] Resolve schema differences if needed
- [x] Add API-only joins / document assembly (optional)
- [x] Keep server unaware of joins
- [x] Add result post-processing (sorting/projection shaping/reconstruction)

## 8. Observability & Debugging
- [x] Collect per-request stats (servers contacted/succeeded/failed)
- [x] Track per-server latency
- [x] Surface rows scanned from server stats
- [x] Add debug modes (consistency checks, verbose logging)
- [x] Ensure no impact on server execution paths

## 9. Resource & Session Management
- [x] Implement Session object
- [x] Store server health state in session
- [x] Store replay log in session
- [x] Store policy configuration in session
- [x] Add best-effort cancellation support

## 10. Testing (API Layer)
- [x] Unit tests: single server
- [x] Unit tests: multiple servers
- [x] Unit tests: failure injection
- [x] Replication tests: partial server failure
- [x] Replication tests: recovery replay
- [x] Replication tests: idempotent retries
- [x] Consistency tests: divergence detection
- [x] Consistency tests: quorum behavior

## 11. Explicit Non-Responsibilities
The API layer does:
- coordination
- fan-out
- retries
- replication
- composition
- joins
- recovery logic

The API layer does not:
- scan data
- interpret storage layout
- execute predicates
- manage locks
- enforce physical consistency

Final Principle (Do Not Violate):
All intelligence lives in the API. The server must remain a dumb, fast,
deterministic execution engine. If a feature feels "smart", it belongs here,
not in the server.

---

# Server Durability TODO (v0.1)

Purpose:
Segment-only durability with forced flush + fsync on segment seal. This is the
only allowed server-side "smart" behavior.

## Durability model
- [x] Document segment-only durability contract (sealed segments durable, active may be lost) (targets: `docs/design.md`)
- [x] Document crash guarantees and recovery behavior (targets: `docs/design.md`)

## Write + flush behavior
- [x] Ensure sealed segments are fsync’d after write (targets: `engine/src/segment_io.cpp`, `server/mimicdb_server.cpp`)
- [x] Ensure metadata/schema is fsync’d after updates (targets: `server/mimicdb_server.cpp`)
- [x] Add optional periodic fsync for active segment (configurable) (targets: `server/mimicdb_server.cpp`)

## Recovery behavior
- [x] Detect and discard partial/active segment files on startup (targets: `server/mimicdb_server.cpp`)
- [x] Resume appends with a fresh active segment after recovery (targets: `server/mimicdb_server.cpp`)
- [x] Add recovery validation tests (sealed survives, active may be lost) (targets: `tests/test_network.py`)

## Config
- [x] Add durability settings to config file (flush_on_seal, flush_interval_ms) (targets: `docs/design.md`, `server/mimicdb_server.cpp`)

---

# Server Namespace TODO (v0.1)

Purpose:
The only server-side awareness beyond raw storage is multiple database
namespaces. The API remains responsible for all coordination semantics.

## Namespaces
- [x] Define database namespace layout on disk (targets: `docs/design.md`)
- [x] Require database name in network requests (targets: `server/mimicdb_server.cpp`, `client/mimicdb_client.py`, `docs/design.md`)
- [x] Maintain per-database dataset registry (targets: `server/mimicdb_server.cpp`)
- [x] Add database create/list operations (targets: `server/mimicdb_server.cpp`, `client/mimicdb_client.py`)
- [x] Update recovery to scan per-database roots (targets: `server/mimicdb_server.cpp`)
- [x] Add namespace isolation tests (targets: `tests/test_network.py`)
- [x] Add database namespace support to the API
