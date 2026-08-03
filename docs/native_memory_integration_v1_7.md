# V1.7 native memory integration

V1.7 makes memory a first-class native MimicRAG capability instead of requiring an application to
assemble a separate vector database and lifecycle service. Memory records use the same durable
catalog, content store, lexical index, and vector index as documents, but remain in a logically
isolated partition. Ordinary retrieval and graph expansion never expose memory records.

## Trust and access model

The server derives the memory owner from the authenticated API-key identity; a request cannot
choose another owner. Tenant and access-scope ACLs still apply. Recall additionally filters active
records by purpose and optional namespace. Sensitive records require confirmation, suspicious
policy/identity/credential text is quarantined, and corrected records become superseded.

`POST /v1/retrieve/combined` returns two separate arrays in this order:

1. `authoritative_documents`
2. `memory_context`

Memory is explicitly lower-trust context and has `policy_authority=false`; it cannot silently
override document evidence.

## Native endpoints

- `POST /v1/memory/remember`: store an evidence-bound record.
- `POST /v1/memory/recall`: semantic and lexical recall for an authorized purpose.
- `POST /v1/memory/inspect`, `/review`, and `/export`: inspect lifecycle state.
- `POST /v1/memory/correct` and `/confirm`: explicit lifecycle mutations.
- `DELETE /v1/memory/{memory_id}`: owner-scoped verified forgetting.
- `POST /v1/retrieve/combined`: coordinated RAG and memory retrieval with fixed trust ordering.

Memory namespaces are `working`, `episodic`, `semantic`, `procedural`, `preference`,
`prospective`, and `negative`. A record must include either inline `evidence` or `evidence_ids`.

## SDK, CLI, and agents

The Python `mimicrag_dev.Client` exposes all native operations. Use
`python -m mimicrag_dev memory review`, `recall`, `inspect`, `confirm`, `forget`, or `export` for
operator review. MCP intentionally exposes only recall, inspect, combined retrieval, and explicit
evidence-bound remember; destructive tools stay outside the autonomous tool surface.

The optional `mimicrag_memory` package remains useful as a richer evidence ledger. Its background
jobs are persisted in SQLite and resume after restart. `LocalHeuristicMemoryModel` is the default
zero-dependency local adapter. `OpenAICompatibleMemoryModel` connects to generic compatible
structured-output endpoints, `AnthropicCompatibleMemoryModel` uses the Anthropic Messages API and
compatible gateways, and `MiniMaxMemoryModel` uses MiniMax's recommended Anthropic-compatible
endpoint. `MiniMaxOpenAICompatibleMemoryModel` is retained as an explicit alternative. All adapters
remain under the existing transmission, sensitivity, redaction, and budget policies.

## Verification

`mimicrag_v17_smoke` verifies partition isolation, owner isolation, semantic recall, combined trust
ordering, confirmation, correction, forgetting, and restart persistence. Python tests cover the
evidence ledger, durable jobs, local extraction, remote-policy fallback, temporal reminders,
deletion, and acceptance metrics.
