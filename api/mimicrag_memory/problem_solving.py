from __future__ import annotations

import re
from typing import Any

from .store import MemoryStore


UNKNOWN_ISSUE_STEPS = [
    {"id": "observe", "action": "Define the observed problem precisely.", "output": "A falsifiable problem statement."},
    {"id": "expected", "action": "Record the expected behavior and the actual behavior.", "output": "A concrete divergence."},
    {"id": "reproduce", "action": "Create the smallest reliable reproduction.", "output": "A repeatable test case."},
    {"id": "evidence", "action": "Gather relevant errors, logs, inputs, outputs, and environment details.", "output": "An evidence set."},
    {"id": "boundary", "action": "Find the earliest point where actual behavior diverges from expected behavior.", "output": "A narrowed failure boundary."},
    {"id": "hypothesis", "action": "Form one testable hypothesis.", "output": "A prediction that evidence can disprove."},
    {"id": "experiment", "action": "Run the smallest safe and reversible test of that hypothesis.", "output": "A recorded result."},
    {"id": "refine", "action": "Accept, refine, or reject the hypothesis from the result.", "output": "An updated explanation."},
    {"id": "validate", "action": "Validate the solution against the original reproduction and likely regressions.", "output": "Evidence that the issue is resolved."},
    {"id": "learn", "action": "Record the validated procedure and any remaining uncertainty as evidence-bound proposals.", "output": "A reviewable procedure candidate."},
]

_SERVICE_USAGE = re.compile(
    r"(?i)\b(api|endpoint|sdk|oauth|webhook|integration|connector|cloud service|hosted service|"
    r"aws|azure|gcp|vercel|stripe|slack|github|anthropic|minimax|openai)\b")


def procedure_for_issue(store: MemoryStore, query: str, *, tenant: str, owner: str,
                        task_kind: str, purpose: str = "coding", minimum_score: float = .6) -> dict[str, Any]:
    """Return learned procedure context or a non-persistent cold-start scaffold."""
    if task_kind != "issue":
        return {"status": "not_applicable", "reason": "fallback is restricted to task_kind=issue"}
    recalled = store.recall(query, tenant=tenant, owner=owner, purpose=purpose,
        namespaces=["procedural"], limit=5)
    relevant = [item for item in recalled["memories"] if item.get("recall_score", 0) >= minimum_score]
    if relevant:
        views = []
        for item in relevant:
            try: views.append(store.refined_procedure(item["id"], tenant=tenant, owner=owner))
            except ValueError: views.append({"source": item, "approved_refinements": [], "immutable_source": True})
        return {"status": "learned_procedure", "procedure_source": "memory", "memories": relevant,
            "procedure_views": views, "fallback_used": False}
    if _SERVICE_USAGE.search(query):
        return {"status": "authoritative_guidance_required", "procedure_source": "none",
            "fallback_used": False, "reason": "service, API, endpoint, or integration usage is excluded from the generic fallback"}
    return {"status": "unknown_issue_scaffold", "procedure_source": "builtin_fallback",
        "procedure_id": "builtin:solve-unknown-v1", "fallback_used": True, "domain_knowledge": False,
        "persistent": False, "steps": UNKNOWN_ISSUE_STEPS,
        "notice": "Investigation scaffold only; it contains no instructions for the specific system and is never stored as learned memory."}
