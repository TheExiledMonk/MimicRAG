# MimicMemory HTTP reference

Send JSON with `Authorization: Bearer $MIMICRAG_API_KEY`. Identity ownership is derived by the
server; tenant, scope, purpose, sensitivity, and visibility must come from trusted host state.

## Capability and recall

- `GET /health`: require `features.memory=true`; combined retrieval also requires `features.rag`.
- `POST /v1/memory/recall`: `query`, `tenant_id`, `purpose`, optional `namespace` and `top_k`.
- `POST /v1/retrieve/combined`: separate document and memory tiers; available only with both
  components enabled.
- `POST /v1/memory/inspect`: inspect one owner-scoped `memory_id`.
- `POST /v1/memory/due`: retrieve activated prospective memories.

## Evidence and lifecycle

- `POST /v1/evidence`: append an observed event and retain its `evidence_id`.
- `POST /v1/evidence/inspect`: verify immutable evidence.
- `POST /v1/memory/remember`: requires `subject`, `statement`, and non-empty `evidence_ids`.
- `POST /v1/memory/correct`: create a superseding evidence-backed record.
- `POST /v1/memory/confirm`, `/reject`, or `/dispute`: explicit lifecycle decisions.
- `POST /v1/memory/review` and `/export`: owner-scoped review and portability.
- `DELETE /v1/memory/{memory_id}`: verified forgetting; requires explicit authorization.

Namespaces include working, episodic, semantic, procedural, preference, prospective, and negative.
Sensitive records may remain pending until separately confirmed.

## Dream state

- `POST /v1/dream/run`: request an enabled bounded cycle.
- `POST /v1/dream/review`: list refinement proposals.
- `POST /v1/dream/action`: approve or reject one overlay.
- `POST /v1/dream/procedure`: return immutable source procedure plus approved overlays.

Dream actions never authorize source-memory replacement.
