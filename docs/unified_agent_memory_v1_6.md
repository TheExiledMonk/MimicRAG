# MimicRAG V1.6 unified agent memory

`mimicrag_memory` is an embedded, evidence-first memory subsystem. It uses a crash-safe WAL ledger
inside the existing process and optionally submits active memory text to MimicRAG under
`memory://` URIs. Memory metadata always carries `trust=memory` and `policy_authority=false`, so
imported documents and inferred memory cannot become identity or policy.

## Data and lifecycle

Every durable memory links to an immutable conversation, tool-result, correction, task-outcome, or
observation event. Records include namespace, subject, canonical statement, event/recording time,
validity, confidence, importance, provenance, reinforcement, decay, visibility, sensitivity,
purposes, and lifecycle status. Supported namespaces are working, episodic, semantic, procedural,
preference, prospective, and negative. Graph edges support `supports`, `contradicts`, `supersedes`,
`caused`, `resolved_by`, `preference_of`, and `remind_when`.

Working state has explicit task/session boundaries, TTLs, bounded context packets, and selective
promotion. Durable recall applies lexical relevance, confidence, importance, reinforcement,
validity, episodic decay, provenance trust, and purpose-specific weighting. Superseded facts remain
available for historical recall. Prospective memories can activate on time, entity, state, or
action context.

## Explicit operations

The Python API exposes `append_evidence`, `remember`, `recall`, `correct`, `forget`, `inspect`, and
`export`. The CLI provides the same core workflow:

```bash
PYTHONPATH=api python -m mimicrag_memory --store memory.db --tenant acme --owner agent event conversation "User prefers concise replies"
PYTHONPATH=api python -m mimicrag_memory --store memory.db --tenant acme --owner agent remember preference style "Prefer concise replies" --evidence EVT_ID
PYTHONPATH=api python -m mimicrag_memory --store memory.db --tenant acme --owner agent recall "response style"
```

Identity-level and sensitive memories require confirmation. Explicit corrections have maximum
confidence and supersede inferred or older records. Forgetting tombstones derived memory,
unreferenced evidence when authorized, associations, and the optional RAG index entry; backup
copies expire under configured snapshot rotation.

## Optional memory model

`MemoryManager` accepts any provider-independent model implementing the small `MemoryModel`
protocol. Remote processing is opt-in per policy and sensitivity; local models bypass remote
transmission. Requests use temperature `0.0`, strict versioned JSON, verified evidence IDs and
quoted spans, redaction, content/time/token/call limits, and transmission auditing. Models only
propose operations. Unsupported, contradictory, low-confidence, sensitive, policy-changing, or
malformed proposals are quarantined. Processing is asynchronous and failure falls back
deterministically without blocking ordinary RAG requests.

The evaluation helper reports useful, missed, intrusive, stale, harmful, and cross-tenant recall,
deletion correctness, latency, token overhead, and remote cost. The regression scenarios cover
correction, temporal history, procedural consolidation, prospective reminders, prompt injection,
privacy boundaries, deletion, model failure, and deterministic caching.
