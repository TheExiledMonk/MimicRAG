# V1.8 memory hardening

V1.8 closes the operational gaps left by the first native memory integration.

## Authoritative evidence

Create evidence with `POST /v1/evidence` before creating or correcting memory. Evidence is stored in
the native catalog under the authenticated owner and tenant. `POST /v1/evidence/inspect` verifies
ownership. Memory operations accept only non-empty `evidence_ids`; unknown and cross-owner IDs are
rejected. Evidence and memory records are excluded from ordinary document retrieval and graph APIs.

## Lifecycle and reminders

Native endpoints now include `/v1/memory/reject`, `/dispute`, and `/due`. Disputes require explicit
source and target memory IDs plus authoritative evidence. Due recall activates prospective records
using time, purpose, and bounded context matching. The embedded store exposes matching confirm,
reject, and review operations. The developer CLI covers evidence append, remember, correct, recall,
review, confirm, reject, dispute, due, export, and forget.

## Durable processing

`MemoryManager` defaults to `LocalHeuristicMemoryModel`. SQLite jobs use atomic claims, worker
leases, three bounded attempts, expired-lease recovery, and a terminal `dead_letter` state. Job
inspection reports attempts, errors, ownership, and lease state. Relation proposals have distinct
`source_id` and `target_id` fields.

## Operator review

The management service proxies review, confirmation, and rejection to MimicRAG using
`MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY`. The web UI Memory tab filters by tenant and lifecycle
status and exposes confirmation/rejection actions. Keep this service behind the same administrative
network and secret controls as the database UI.

## Evaluation

The configurable soak scenario uses `MIMICRAG_MEMORY_SOAK_SESSIONS` (250 by default) and measures
useful, missed, intrusive, stale, harmful, leakage, deletion, token, cost, and p50/p95/p99 latency
metrics. Provider protocol tests are deterministic; live Anthropic and MiniMax contracts are opt-in
through `ANTHROPIC_API_KEY`, `MINIMAX_API_KEY`, and `MINIMAX_OPENAI_LIVE`.
