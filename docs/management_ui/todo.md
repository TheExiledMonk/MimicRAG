# MimicDB Management UI Todo (v0)

## Project setup
- [x] Choose repo structure for UI + service: new top-level `management_ui/` with `service/` + `ui/`.
- [x] Decide Python service package name: `mimicdb_ui_service`.
- [x] Decide UI stack and tooling: React + Vite + TypeScript.
- [x] Decide desktop wrapper approach: pywebview.
- [x] Define versioning + release target: v0 desktop bundle, starting at `0.1.0`.

## Service API (FastAPI)
- [x] Add service app skeleton with config (host, port, default database).
- [x] Implement `/health` endpoint with version reporting.
- [x] Implement `/connect` endpoint (host/port/database) and connection management.
- [x] Implement `/databases` endpoint using `client/mimicdb_client.py`.
- [x] Implement `/datasets` endpoint (database param).
- [x] Implement `/schema` endpoint with field metadata + encoding info.
- [x] Implement `/scan` endpoint with cursor-based paging (no offsets).
- [x] Define cursor token format and server-side state for scan paging.
- [x] Implement `/aggregate` endpoint (single dataset, single field).
- [x] Implement mutations: `/create_database`, `/drop_database`.
- [x] Implement mutations: `/create_dataset`, `/drop_dataset`.
- [x] Implement `/append` for explicit small-batch append.
- [x] Add validation for predicates (type checks, supported ops).
- [x] Add validation for field types and unsupported aggregates.
- [x] Ensure all endpoints default to localhost-only unless configured.

## UI (web app)
- [x] Implement layout: sidebar (connection + DB list + dataset list), main tabs.
- [x] Add connection form (host/port/db) and connection status.
- [x] Add database selector and dataset list (refresh control).
- [x] Add Schema tab (field name, type, nullable, encoding).
- [x] Add Data Preview tab (column multi-select, predicate builder, paging).
- [x] Render nulls explicitly as `NULL` with subtle styling.
- [x] Add Aggregate tab (single field selection, manual trigger).
- [x] Add Ingest tab (optional): append small batch with field inputs.
- [x] Add Settings tab (max scan limit, bind info, toggles).
- [x] Add Admin tab for creating/dropping databases and datasets.

## Query styles
- [x] Implement Mimic Native builder (predicates + aggregates).
- [x] Implement Mongo-style builder (`$eq`, `$gt`, `$and`) mapped to predicates.
- [x] Implement SQL-style restricted parser (SELECT/FROM/WHERE/LIMIT).
- [x] Ensure all query styles map to the same `/scan` and `/aggregate` APIs.

## Data preview rules
- [x] Enforce default limit (100) and configurable hard cap (e.g., 10k).
- [x] Enforce cursor-only pagination (no offset requests).
- [x] Disable auto-refresh; use explicit user actions only.
- [x] Add guardrails for full scans (confirmations or hard limits).

## Type rendering
- [x] Render `dict_int32` as decoded values by default.
- [x] Add toggle to show dictionary IDs.
- [x] Truncate strings to ~128 chars with tooltip for full value.
- [x] Render bytes as hex preview + length indicator.

## CSV export
- [x] Export current preview page only.
- [x] Add warning if truncated.
- [x] Add options: include headers, null rendering choice.

## Desktop packaging
- [x] Wire pywebview to launch the Python service.
- [x] Ensure service binds to localhost by default.
- [x] Define single-binary build process (platform-specific).
- [ ] Smoke test on target OS (launch, connect, browse, scan, aggregate).

## Security + reliability
- [x] Add basic error mapping (network errors, protocol errors, timeouts).
- [x] Ensure errors are surfaced in UI with actionable messages.
- [x] Add safe defaults: localhost bind, no remote exposure unless configured.
- [x] Add logging for service requests and failures.

## Performance guardrails
- [x] Cache schema in UI.
- [x] Avoid large payloads; consider chunked responses or streaming for scans.
- [x] Add hard limits for scan result size and payload size.

## Testing
- [x] Add service unit tests for API endpoints and predicate validation.
- [x] Add UI tests for paging, filters, and error states.
- [x] Add integration smoke test (service + UI) against a local MimicDB server.

## Definition of done (v0)
- [ ] Browse databases and datasets.
- [ ] View schema with encoding info.
- [ ] Preview data safely with predicates and cursor paging.
- [ ] Run aggregates with explicit trigger.
- [ ] Mutations (create/drop dataset/db, append) work by explicit action.
- [ ] Desktop build runs offline and does not require server changes.
