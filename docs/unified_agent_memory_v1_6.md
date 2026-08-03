# MimicRAG V1.6 unified agent memory

`mimicrag_memory` is an optional embedded Python subsystem. It is not compiled into the native C++
server and it does not store its ledger in MimicDB. It uses a local SQLite WAL database for
evidence, memory records, relations, audit history, working state, and model runs. Active memory
can optionally be indexed through the existing MimicRAG HTTP API under `memory://` source URIs.

This boundary matters operationally: the native server can run without Python or memory, and the
memory ledger requires its own lifecycle, permissions, backup, and restore plan.

## Installation and storage

```bash
python -m pip install -e ./api
install -d -m 0700 /var/lib/mimicrag-memory
```

Open one `MemoryStore` per ledger path and keep the file on a local filesystem with reliable SQLite
locking. Avoid network filesystems unless they explicitly support SQLite WAL semantics.

```python
from mimicrag_dev import Client
from mimicrag_memory import MemoryStore

rag = Client("https://rag.example.com", api_key="...")
store = MemoryStore("/var/lib/mimicrag-memory/memory.db", rag_client=rag)
```

The RAG client is optional. Without it, recall uses the ledger's deterministic lexical and metadata
ranking. With it, active memory is also submitted to MimicRAG with `trust=memory` and
`policy_authority=false`. Combined recall always places authoritative document evidence before
memory context.

## Evidence-first model

Every durable memory links to an immutable conversation, tool-result, correction, task-outcome, or
observation event. Records contain namespace, subject, canonical statement, event and recording
time, validity interval, confidence, importance, provenance, reinforcement, decay, visibility,
sensitivity, allowed purposes, and lifecycle status.

Namespaces are `working`, `episodic`, `semantic`, `procedural`, `preference`, `prospective`, and
`negative`. Relations are `supports`, `contradicts`, `supersedes`, `caused`, `resolved_by`,
`preference_of`, and `remind_when`.

Imported or inferred text containing identity, policy, or credential-changing instructions is
quarantined. Memory remains data; applications must never concatenate it into system or developer
instructions.

## Explicit Python API

```python
from mimicrag_memory import MemoryNamespace, MemoryRecord, Visibility

event_id = store.append_evidence(
    tenant="acme",
    owner="agent-7",
    kind="conversation",
    content="User prefers concise status updates.",
    provenance="chat:session-42",
    sensitivity="internal",
    purpose="conversation",
)

memory = store.remember(MemoryRecord(
    tenant="acme",
    owner="agent-7",
    namespace=MemoryNamespace.PREFERENCE,
    subject="response style",
    statement="The user prefers concise status updates.",
    visibility=Visibility.PRIVATE,
    sensitivity="internal",
    allowed_purposes=["conversation", "planning"],
    evidence_ids=[event_id],
    confidence=0.9,
    importance=0.7,
))

packet = store.recall(
    "How should I report progress?",
    tenant="acme",
    owner="agent-7",
    purpose="conversation",
    limit=8,
)
audit = store.inspect(memory["id"], tenant="acme", owner="agent-7")
```

Other synchronous operations are `correct`, `reinforce`, `associate`, `prospective`,
`set_working`, `context_packet`, `promote_working`, `forget`, `export`, and `audit`. Always pass
tenant and owner from authenticated application state—not from model-generated arguments.

Sensitive, personal, legal, financial, and identity-level records enter
`pending_confirmation` unless `confirmed=True` is supplied by a trusted approval path. Explicit
corrections supersede older records and receive confidence `1.0`; superseded records remain
available only when historical recall is requested.

## CLI

```bash
python -m mimicrag_memory --store memory.db --tenant acme --owner agent \
  event conversation "User prefers concise replies"
python -m mimicrag_memory --store memory.db --tenant acme --owner agent \
  remember preference style "Prefer concise replies" --evidence EVT_ID
python -m mimicrag_memory --store memory.db --tenant acme --owner agent \
  recall "response style" --purpose conversation
python -m mimicrag_memory --store memory.db --tenant acme --owner agent \
  inspect MEMORY_ID
python -m mimicrag_memory --store memory.db --tenant acme --owner agent \
  export > memory-export.json
```

The CLI is an administrative interface, not an authenticated network service. Restrict filesystem
and process access accordingly.

## Optional memory model

`MemoryManager` accepts a provider-independent object with `provider`, `model`, `local`, and a
`propose(request, temperature, timeout)` method. It is separate from the native `chat` provider
configuration; applications may wrap the same provider or choose a cheaper/local specialist.
V1.7 includes local heuristic, generic OpenAI-compatible, Anthropic-compatible, and MiniMax
adapters. MiniMax defaults to its recommended Anthropic-compatible API; an OpenAI-compatible
MiniMax adapter remains available explicitly.

```python
from mimicrag_memory import MemoryManager, MemoryPolicy

policy = MemoryPolicy(
    remote_processing=True,
    allowed_remote_sensitivity={"public", "internal"},
    provider_retention="none",
    residency="us",
    maximum_calls_per_session=2,
    maximum_tokens_per_session=4000,
)
manager = MemoryManager(store, model=remote_model, local_fallback=local_model, policy=policy)
job_id = manager.submit_boundary(
    tenant="acme",
    owner="agent-7",
    evidence_ids=[event_id],
    purpose="conversation",
    operation="extract",
)
```

Remote processing is off by default. Requests use temperature `0.0`, strict versioned JSON,
verified evidence IDs and quoted spans, configured redaction, call/token/content limits, and an
exact transmission audit. Models only propose operations. Unsupported, low-confidence, sensitive,
policy-changing, or malformed proposals are quarantined. Sensitive evidence selects a configured
local fallback or is rejected. Call `manager.shutdown()` and `store.close()` during clean shutdown.

## Working state, recall, and evaluation

Working state has explicit session/task IDs, TTLs, a fixed field allowlist, bounded context packets,
and selective promotion through `selected_outcomes`. Episodic recall decays by age without deleting
evidence. Purpose profiles favor different namespaces for coding, planning, conversation, and
research. Prospective memory can activate from time plus entity/state/action terms.

`mimicrag_memory.evaluation.evaluate_cases` reports useful, missed, intrusive, harmful, and
cross-tenant recall, latency, token overhead, and deletion acceptance fields. It is a deterministic
offline harness; applications remain responsible for representative long-running scenarios and
provider cost inputs.

## Forgetting and backup boundaries

`forget` tombstones the memory, removes relation visibility, invalidates the optional RAG document,
and may tombstone source evidence only when no other live memory references it. SQLite pages and RAG
derived indexes are reclaimed according to their own compaction/checkpoint lifecycle. Existing
backups are not rewritten; expire or destroy them under the deployment's backup-retention policy.

For a consistent ledger backup, stop memory writers or use the SQLite backup API. Copying only
`memory.db` while a writer is active can omit committed WAL content. The native MimicRAG snapshot
does not include a ledger stored elsewhere. If the ledger is placed directly under
`server.data_path`, native snapshots copy regular files only and still do not coordinate SQLite
WAL checkpoints, so a separate SQLite-aware backup remains recommended.

## Historical V1.6 integration limits

- V1.6 had no native `/v1/memory/*` routes; V1.7 adds authenticated native routes.
- V1.6 recall was lexical/metadata-based unless the optional RAG client was used; V1.7 adds native semantic recall.
- Adapters use the Python standard library and do not require provider SDK packages.
- Visibility values are stored and audited, but the application remains responsible for mapping
  authenticated principals to allowed visibility levels.
- Backup erasure is governed by external retention/rotation policy rather than immediate mutation
  of immutable snapshots.
