# V1.9 optional dream state

Dream state is an opt-in MimicMemory maintenance cycle that categorizes memory, extracts structure
from procedural memories, detects conflicts and staleness, and proposes refinements. It never
replaces or rewrites a source memory. Approved refinements remain overlays attached to the immutable
procedure and can be inspected independently.

Dream maintenance covers the complete memory model, not only procedures. Semantic facts may be
categorized as profile, project, research, or general facts; preferences, commitments, experiences,
cautions, and current-task memories retain their existing namespaces and lifecycle rules. Procedure
extraction is an additional capability for explicit procedural memories or multi-step processes
discovered inside other memory types.

Dream state is disabled by default. A local cycle requires an explicit `--enable`:

```bash
PYTHONPATH=api python -m mimicrag_memory \
  --store mimicrag-memory.db --tenant TENANT --owner OWNER \
  dream --enable --mode deep
```

Review and approve or reject individual suggestions:

```bash
PYTHONPATH=api python -m mimicrag_memory --store mimicrag-memory.db --tenant TENANT --owner OWNER refinements --status pending_review
PYTHONPATH=api python -m mimicrag_memory --store mimicrag-memory.db --tenant TENANT --owner OWNER review-refinement REFINE_ID approved
PYTHONPATH=api python -m mimicrag_memory --store mimicrag-memory.db --tenant TENANT --owner OWNER refined-procedure MEMORY_ID
```

Allowed proposal operations are categorization, annotation, linking, step insertion/splitting,
preconditions, validation, recovery, parameterization, and stale/conflict flags. Replacement,
deletion, evidence rewriting, silent promotion, and external action execution are forbidden.

## Optional research

Research mode is separately disabled. It requires both explicit research mode and a user-controlled
JSON research endpoint:

```bash
MIMICRAG_DREAM_RESEARCH_URL=https://research-gateway.example/v1/search \
MIMICRAG_DREAM_RESEARCH_KEY=... \
PYTHONPATH=api python -m mimicrag_memory \
  --store mimicrag-memory.db --tenant TENANT --owner OWNER \
  dream --enable --mode research --authoritative-domain docs.example.com
```

The endpoint receives `query`, `maximum_sources`, and `authoritative_domains`, and returns a
`sources` array containing `url`, `title`, and `summary` or `content`. Network request count,
sources per request, cycle duration, reviewed memories, and suggestions are bounded by
`DreamPolicy`.

External findings are stored as immutable evidence with retrieval time, URL, and
`untrusted_external: true`. They can support a refinement proposal but never outrank local policy,
rewrite a procedure, or trigger an action. An LLM is not required for local light/deep cycles;
an external research service is required only for research mode.

Every wake-up report records reviewed memories, procedures found, suggestions, research calls,
duration, and explicit zero counts for source mutation, automatic promotion, and external action.

## Native HTTP and operator review

The native server persists deterministic refinement records in a separate internal partition:

- `POST /v1/dream/run`
- `POST /v1/dream/review`
- `POST /v1/dream/action`
- `POST /v1/dream/procedure`

All routes bind ownership to the authenticated API identity. Ordinary RAG retrieval excludes
refinement records. The management UI Memory tab can start a native cycle, filter refinements, and
approve or reject overlays.

## Scheduling, built-in search, models, and automatic approval

Run an explicitly enabled interval worker with:

```bash
PYTHONPATH=api python -m mimicrag_memory --store mimicrag-memory.db --tenant TENANT --owner OWNER \
  dream-schedule --enable --interval 3600 --mode deep --auto-approve categorize
```

`BuiltinWebResearcher` provides zero-key bounded DuckDuckGo HTML search. Enable it with
`dream --enable --mode research --builtin-search`; results remain untrusted evidence. Deployments
may still use `JsonSearchResearcher` for controlled gateways.

`DreamEngine(..., model=MODEL)` accepts optional model-assisted refinements when
`DreamPolicy.model_enabled` is true. Output is restricted to the refinement allowlist, target and
evidence IDs must already belong to the reviewed memory set, and model suggestions always remain
pending review.

Automatic approval is a policy overlay, not autonomous memory mutation. Only configured operations
with confidence at least `0.9` qualify. Model-assisted and research-backed suggestions are never
automatically approved. The recommended and CLI-supported automatic operation is deterministic
`categorize`; approval still leaves the source record unchanged.
