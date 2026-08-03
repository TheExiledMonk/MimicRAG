# V1.9.1 unknown-issue fallback

When an agent is explicitly solving an issue and MimicMemory has no sufficiently relevant learned
procedure, it can return a built-in investigation scaffold. The scaffold does not contain domain
knowledge and is not presented as learned memory. It guides observation, reproduction, evidence
collection, failure-boundary isolation, hypothesis testing, validation, and evidence-bound learning.

```bash
PYTHONPATH=api python -m mimicrag_memory \
  --store mimicrag-memory.db --tenant TENANT --owner OWNER \
  solve-unknown "Output becomes corrupted after the third retry" \
  --task-kind issue
```

Selection follows this order:

1. Return a sufficiently relevant procedural memory if one exists.
2. If the task is explicitly an issue and is not service-specific, return
   `builtin:solve-unknown-v1`.
3. For service, API, endpoint, SDK, authentication, webhook, cloud, or integration usage, return
   `authoritative_guidance_required` instead of the generic scaffold.
4. For non-issue tasks, return `not_applicable`.

The fallback is a constant, non-persistent view. It never inserts or reinforces a memory, invents
commands or endpoints, performs research, or executes actions. Only a subsequently validated,
task-specific process backed by real evidence may become a normal procedural-memory proposal.
