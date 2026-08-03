from __future__ import annotations

import json
import html
import re
import threading
import time
import urllib.parse
import urllib.request
import uuid
from dataclasses import asdict, dataclass, field
from typing import Any, Protocol

from .store import MemoryStore


ALLOWED_REFINEMENTS = {"categorize", "annotate", "link", "insert_before", "insert_after",
    "split_step", "add_precondition", "add_validation", "add_recovery", "parameterize",
    "flag_stale", "flag_conflict"}
FORBIDDEN_REFINEMENTS = {"replace_procedure", "delete_memory", "rewrite_evidence", "silently_promote", "execute_external_action"}


@dataclass
class DreamPolicy:
    enabled: bool = False
    research_enabled: bool = False
    maximum_memories: int = 500
    maximum_suggestions: int = 100
    maximum_research_queries: int = 3
    maximum_sources_per_query: int = 3
    maximum_seconds: float = 30.0
    stale_after_days: int = 180
    authoritative_domains: list[str] = field(default_factory=list)
    model_enabled: bool = False
    maximum_model_proposals: int = 20
    auto_approve_operations: set[str] = field(default_factory=set)


class Researcher(Protocol):
    def search(self, query: str, *, maximum_sources: int, authoritative_domains: list[str]) -> list[dict[str, Any]]: ...


class JsonSearchResearcher:
    """Optional adapter for a user-controlled JSON search/research endpoint."""
    def __init__(self, endpoint: str, api_key: str = "", timeout: float = 15.0):
        self.endpoint, self.api_key, self.timeout = endpoint, api_key, timeout

    def search(self, query: str, *, maximum_sources: int, authoritative_domains: list[str]) -> list[dict[str, Any]]:
        body = json.dumps({"query": query, "maximum_sources": maximum_sources,
            "authoritative_domains": authoritative_domains}).encode()
        headers = {"Content-Type": "application/json"}
        if self.api_key: headers["Authorization"] = "Bearer " + self.api_key
        request = urllib.request.Request(self.endpoint, body, headers, method="POST")
        with urllib.request.urlopen(request, timeout=self.timeout) as response: payload = json.load(response)
        sources = payload.get("sources", [])
        if not isinstance(sources, list): raise ValueError("research endpoint must return sources")
        return sources[:maximum_sources]


class BuiltinWebResearcher:
    """Zero-key bounded web search using DuckDuckGo's HTML results."""
    def __init__(self, timeout: float = 15.0): self.timeout = timeout
    def search(self, query: str, *, maximum_sources: int, authoritative_domains: list[str]) -> list[dict[str, Any]]:
        scoped = query + (" " + " OR ".join("site:" + domain for domain in authoritative_domains) if authoritative_domains else "")
        url = "https://html.duckduckgo.com/html/?" + urllib.parse.urlencode({"q": scoped})
        request = urllib.request.Request(url, headers={"User-Agent": "MimicMemory/1.9 research"})
        with urllib.request.urlopen(request, timeout=self.timeout) as response: page = response.read(1_000_000).decode("utf-8", "replace")
        links = re.findall(r'class="result__a"[^>]*href="([^"]+)"[^>]*>(.*?)</a>', page, re.I | re.S)
        snippets = re.findall(r'class="result__snippet"[^>]*>(.*?)</(?:a|div)>', page, re.I | re.S)
        results = []
        for index, (target, title) in enumerate(links[:maximum_sources]):
            parsed = urllib.parse.urlparse(html.unescape(target)); query_args = urllib.parse.parse_qs(parsed.query)
            resolved = query_args.get("uddg", [target])[0]
            clean = lambda value: html.unescape(re.sub(r"<[^>]+>", " ", value)).strip()
            results.append({"url": resolved, "title": clean(title), "summary": clean(snippets[index]) if index < len(snippets) else "Search result relevant to " + query})
        return results


def _steps(statement: str) -> list[str]:
    parts = re.split(r"(?:\n+|(?<=[.!?])\s+|\s+(?:then|after that|finally)\s+)", statement, flags=re.IGNORECASE)
    return [part.strip(" -\t.") for part in parts if len(part.strip(" -\t.")) >= 4]


def _category(memory: dict[str, Any]) -> str:
    text = (memory["subject"] + " " + memory["statement"]).lower()
    if memory["namespace"] == "procedural" or len(_steps(memory["statement"])) > 1: return "procedure"
    if any(term in text for term in ("name", "country", "timezone", "language", "location")): return "profile"
    if any(term in text for term in ("research", "study", "paper", "source", "hypothesis")): return "research"
    if any(term in text for term in ("book", "novel", "project", "manuscript", "release")): return "project"
    return {"preference": "preference", "prospective": "commitment", "episodic": "experience",
        "negative": "caution", "working": "current_task", "semantic": "fact"}.get(memory["namespace"], "general")


class DreamEngine:
    """Opt-in reflection engine. It proposes refinements and never mutates source memories."""
    def __init__(self, store: MemoryStore, *, policy: DreamPolicy | None = None, researcher: Researcher | None = None, model: Any = None):
        self.store, self.policy, self.researcher, self.model = store, policy or DreamPolicy(), researcher, model

    def run(self, *, tenant: str, owner: str, mode: str = "light") -> dict[str, Any]:
        if not self.policy.enabled: raise ValueError("dream state is disabled")
        if mode not in {"light", "deep", "research"}: raise ValueError("dream mode must be light, deep, or research")
        if mode == "research" and (not self.policy.research_enabled or not self.researcher):
            raise ValueError("dream research is disabled or no researcher is configured")
        cycle_id, started = "dream-" + uuid.uuid4().hex, time.monotonic()
        rows = self.store.db.execute("SELECT record FROM memories WHERE tenant=? AND owner=? AND status='active' ORDER BY updated_at_ms DESC LIMIT ?",
            (tenant, owner, self.policy.maximum_memories)).fetchall()
        memories = [json.loads(row[0]) for row in rows]; suggestions: list[dict[str, Any]] = []; research_queries = 0
        procedural = [item for item in memories if item["namespace"] == "procedural" or len(_steps(item["statement"])) > 1]
        category_budget = max(1, self.policy.maximum_suggestions // 2)
        for item in memories[:category_budget]:
            suggestions.append(self._proposal(cycle_id, item, "categorize", {"category": _category(item),
                "namespace": item["namespace"]}, "Categorized memory for lifecycle maintenance without changing its namespace",
                item["evidence_ids"], .9))
        for item in procedural:
            if len(suggestions) >= self.policy.maximum_suggestions or time.monotonic() - started > self.policy.maximum_seconds: break
            steps = _steps(item["statement"])
            if len(steps) > 1:
                suggestions.append(self._proposal(cycle_id, item, "split_step", {"source_text": item["statement"], "steps": steps},
                    "The source contains multiple ordered actions", item["evidence_ids"], .9))
            lowered = item["statement"].lower()
            if not any(word in lowered for word in ("verify", "check", "test", "validate")):
                suggestions.append(self._proposal(cycle_id, item, "add_validation", {"proposed_step": "Verify the expected outcome before continuing."},
                    "No explicit validation step was detected", item["evidence_ids"], .65))
            if not any(word in lowered for word in ("fail", "rollback", "recover", "retry")):
                suggestions.append(self._proposal(cycle_id, item, "add_recovery", {"proposed_step": "Stop safely and record the failure for review."},
                    "No failure-recovery instruction was detected", item["evidence_ids"], .6))
            age_days = max(0, int(time.time() * 1000) - item["recorded_at_ms"]) / 86400000
            if age_days >= self.policy.stale_after_days:
                suggestions.append(self._proposal(cycle_id, item, "flag_stale", {"age_days": age_days},
                    "The procedure exceeds the configured freshness interval", item["evidence_ids"], .8))
                if mode == "research" and research_queries < self.policy.maximum_research_queries:
                    research_queries += 1
                    sources = self.researcher.search("Current authoritative guidance for: " + item["subject"],
                        maximum_sources=self.policy.maximum_sources_per_query,
                        authoritative_domains=self.policy.authoritative_domains)
                    evidence_ids = list(item["evidence_ids"])
                    for source in sources:
                        content = str(source.get("summary") or source.get("content") or "").strip()
                        url = str(source.get("url", "")).strip()
                        if not content or not url: continue
                        evidence_ids.append(self.store.append_evidence(tenant=tenant, owner=owner, kind="observation",
                            content=content, provenance="dream_research:" + url, purpose="research",
                            metadata={"url": url, "title": source.get("title", ""), "retrieved_at_ms": int(time.time() * 1000), "untrusted_external": True}))
                    if len(evidence_ids) > len(item["evidence_ids"]):
                        suggestions.append(self._proposal(cycle_id, item, "annotate", {"research_evidence_ids": evidence_ids[len(item["evidence_ids"]):]},
                            "External findings are attached as untrusted evidence for review", evidence_ids, .5))
        subjects: dict[str, list[dict[str, Any]]] = {}
        for item in memories: subjects.setdefault(item["subject"].strip().lower(), []).append(item)
        for group in subjects.values():
            statements = {item["statement"].strip().lower() for item in group}
            if len(group) > 1 and len(statements) > 1:
                for item in group[:2]:
                    suggestions.append(self._proposal(cycle_id, item, "flag_conflict", {"related_memory_ids": [other["id"] for other in group if other["id"] != item["id"]]},
                        "Multiple active memories share a subject but differ in content", item["evidence_ids"], .7))
        if self.policy.model_enabled and self.model and len(suggestions) < self.policy.maximum_suggestions:
            request = {"schema_version": 1, "operation": "refine_procedures", "rules": {
                "allowed_operations": sorted(ALLOWED_REFINEMENTS), "forbidden_operations": sorted(FORBIDDEN_REFINEMENTS),
                "source_memory_mutation": False}, "memories": [{"id": item["id"], "namespace": item["namespace"],
                "subject": item["subject"], "statement": item["statement"], "evidence_ids": item["evidence_ids"]}
                for item in memories[:50]]}
            output = self.model.propose(request, temperature=0.0, timeout=min(self.policy.maximum_seconds, 30))
            known = {item["id"]: item for item in memories}
            for raw in output.get("refinements", [])[:self.policy.maximum_model_proposals]:
                memory = known.get(raw.get("memory_id")); operation = raw.get("operation")
                if not memory or operation not in ALLOWED_REFINEMENTS or operation in FORBIDDEN_REFINEMENTS: continue
                if not isinstance(raw.get("patch"), dict) or not 0 <= float(raw.get("confidence", 0)) <= 1: continue
                evidence_ids = raw.get("evidence_ids", memory["evidence_ids"])
                if any(identifier not in memory["evidence_ids"] for identifier in evidence_ids): continue
                suggestions.append(self._proposal(cycle_id, memory, operation, raw["patch"],
                    "Model-assisted suggestion: " + str(raw.get("reason", "review required")), evidence_ids, float(raw.get("confidence", 0))))
        suggestions = suggestions[:self.policy.maximum_suggestions]
        report = {"cycle_id": cycle_id, "mode": mode, "reviewed": len(memories), "procedures_found": len(procedural),
            "suggestions": len(suggestions), "research_queries": research_queries, "duration_ms": (time.monotonic() - started) * 1000,
            "safety": {"source_memories_modified": 0, "automatic_promotions": 0, "external_actions_executed": 0}}
        policy_value = asdict(self.policy); policy_value["auto_approve_operations"] = sorted(self.policy.auto_approve_operations)
        self.store.save_dream_cycle(cycle_id, tenant=tenant, owner=owner, mode=mode, policy=policy_value, report=report, suggestions=suggestions)
        auto_approved = 0
        for item in suggestions:
            if item["operation"] in self.policy.auto_approve_operations and item["confidence"] >= .9 and not item["reason"].startswith("Model-assisted") and not item["patch"].get("research_evidence_ids"):
                self.store.review_refinement(item["id"], tenant=tenant, owner=owner, decision="approved", reason="dream policy auto-approval")
                item["status"] = "approved"; auto_approved += 1
        report["auto_approved"] = auto_approved
        return {**report, "refinements": suggestions}

    def _proposal(self, cycle_id: str, memory: dict[str, Any], operation: str, patch: dict[str, Any],
                  reason: str, evidence_ids: list[str], confidence: float) -> dict[str, Any]:
        if operation not in ALLOWED_REFINEMENTS or operation in FORBIDDEN_REFINEMENTS: raise ValueError("unsafe refinement operation")
        return {"id": "refine-" + uuid.uuid4().hex, "cycle_id": cycle_id, "memory_id": memory["id"],
            "operation": operation, "patch": patch, "reason": reason, "evidence_ids": list(dict.fromkeys(evidence_ids)),
            "confidence": confidence, "status": "pending_review", "created_at_ms": int(time.time() * 1000)}


class DreamScheduler:
    """Optional interval scheduler; disabled until explicitly started."""
    def __init__(self, engine: DreamEngine, *, tenant: str, owner: str, interval_seconds: float = 3600, mode: str = "light"):
        if interval_seconds < .01: raise ValueError("dream interval must be at least 0.01 seconds")
        self.engine, self.tenant, self.owner, self.interval_seconds, self.mode = engine, tenant, owner, interval_seconds, mode
        self.stop_event = threading.Event(); self.last_report: dict[str, Any] | None = None; self.last_error = ""
        self.thread = threading.Thread(target=self._run, name="mimicmemory-dream", daemon=True)
    def start(self) -> None: self.thread.start()
    def _run(self) -> None:
        while not self.stop_event.wait(self.interval_seconds):
            try: self.last_report = self.engine.run(tenant=self.tenant, owner=self.owner, mode=self.mode); self.last_error = ""
            except Exception as exc: self.last_error = str(exc)
    def stop(self) -> None:
        self.stop_event.set()
        if self.thread.is_alive(): self.thread.join(timeout=2)
