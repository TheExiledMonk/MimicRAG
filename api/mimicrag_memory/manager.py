from __future__ import annotations

import hashlib
import json
import queue
import re
import threading
import time
import uuid
from dataclasses import asdict
from typing import Any, Protocol

from .models import MemoryNamespace, MemoryPolicy, MemoryProposal, MemoryRecord, SENSITIVE_CLASSES, Visibility
from .store import MemoryStore


class MemoryModel(Protocol):
    provider: str
    model: str
    local: bool
    def propose(self, request: dict[str, Any], *, temperature: float = 0.0, timeout: float = 30.0) -> dict[str, Any]: ...


class MemoryManager:
    """Optional asynchronous model manager. Models propose; validated native operations mutate."""

    def __init__(self, store: MemoryStore, model: MemoryModel | None = None, *, local_fallback: MemoryModel | None = None, policy: MemoryPolicy | None = None,
                 prompt_version: str = "memory-manager-v1", schema_version: int = 1):
        self.store = store; self.model = model; self.local_fallback = local_fallback; self.policy = policy or MemoryPolicy(); self.prompt_version = prompt_version; self.schema_version = schema_version
        self.budgets: dict[str, dict[str, int]] = {}
        self.queue: queue.Queue[tuple[str, dict[str, Any]]] = queue.Queue(maxsize=128); self.stop_event = threading.Event()
        self.worker = threading.Thread(target=self._run, name="mimicrag-memory", daemon=True); self.worker.start()

    def _redact(self, content: str) -> str:
        for pattern in self.policy.secret_patterns: content = re.sub(pattern, "[REDACTED]", content)
        return content[: self.policy.maximum_content_chars]

    def _evidence_payload(self, tenant: str, owner: str, evidence_ids: list[str], purpose: str, model: MemoryModel) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
        payload, transmission = [], []
        for identifier in evidence_ids:
            row = self.store._evidence(identifier, tenant, owner)
            if row["purpose"] != purpose and purpose not in {"consolidation", "reflection"}: raise ValueError("evidence purpose is not authorized")
            if not model.local and (not self.policy.remote_processing or row["sensitivity"] not in self.policy.allowed_remote_sensitivity):
                raise ValueError("remote processing is not allowed for evidence sensitivity")
            content = self._redact(row["content"])
            payload.append({"id": identifier, "kind": row["kind"], "content": content, "event_time_ms": row["event_time_ms"]})
            transmission.append({"evidence_id": identifier, "provider": model.provider, "model": model.model, "local": model.local,
                "policy": {"retention": self.policy.provider_retention, "residency": self.policy.residency}, "characters": len(content)})
        return payload, transmission

    def submit_boundary(self, *, tenant: str, owner: str, evidence_ids: list[str], purpose: str = "conversation",
                        operation: str = "extract") -> str:
        if operation not in {"extract", "consolidate", "reflect", "associate", "decay"}: raise ValueError("unsupported memory job")
        identifier = "mjob-" + uuid.uuid4().hex
        self.queue.put_nowait((identifier, {"tenant": tenant, "owner": owner, "evidence_ids": evidence_ids, "purpose": purpose, "operation": operation}))
        return identifier

    def _run(self) -> None:
        while not self.stop_event.is_set():
            try: identifier, request = self.queue.get(timeout=0.1)
            except queue.Empty: continue
            try: self.process(**request)
            except Exception as exc:
                with self.store.db:
                    self.store._audit(request["tenant"], request["owner"], "memory.job_failed", identifier, str(exc))
            finally: self.queue.task_done()

    def process(self, *, tenant: str, owner: str, evidence_ids: list[str], purpose: str,
                operation: str = "extract", session_id: str = "default") -> dict[str, Any]:
        if not self.model: return {"status": "deterministic_fallback", "accepted": [], "reason": "memory model unavailable"}
        processing_model = self.model
        if not processing_model.local:
            sensitivities = [self.store._evidence(identifier, tenant, owner)["sensitivity"] for identifier in evidence_ids]
            if not self.policy.remote_processing or any(value not in self.policy.allowed_remote_sensitivity for value in sensitivities):
                if self.local_fallback and self.local_fallback.local: processing_model = self.local_fallback
                else: raise ValueError("remote processing is not allowed for evidence sensitivity")
        evidence, transmission = self._evidence_payload(tenant, owner, evidence_ids, purpose, processing_model)
        budget = self.budgets.setdefault(session_id, {"calls": 0, "tokens": 0, "accepted": 0})
        estimated_tokens = sum(len(item["content"]) for item in evidence) // 4
        if budget["calls"] >= self.policy.maximum_calls_per_session or budget["tokens"] + estimated_tokens > self.policy.maximum_tokens_per_session:
            return {"status": "budget_exhausted", "accepted": []}
        budget["calls"] += 1; budget["tokens"] += estimated_tokens
        cache_key = hashlib.sha256(json.dumps({"hashes": [hashlib.sha256(item["content"].encode()).hexdigest() for item in evidence],
            "model": processing_model.model, "prompt": self.prompt_version, "schema": self.schema_version, "operation": operation}, sort_keys=True).encode()).hexdigest()
        cached = self.store.db.execute("SELECT validation,mutation_ids FROM model_runs WHERE cache_key=?", (cache_key,)).fetchone()
        if cached: return {"status": "cached", "validation": cached[0], "accepted": json.loads(cached[1])}
        request = {"schema_version": self.schema_version, "prompt_version": self.prompt_version, "operation": operation,
            "rules": {"evidence_ids_required": True, "quoted_spans_required": True, "policy_authority": False,
                      "maximum_memories": self.policy.maximum_accepted_memories, "maximum_associations": self.policy.maximum_associations},
            "evidence": evidence}
        started = time.monotonic(); output: dict[str, Any] = {}; accepted: list[str] = []; validation = "accepted"
        try:
            try: output = processing_model.propose(request, temperature=0.0, timeout=self.policy.timeout_seconds)
            except Exception:
                if processing_model is self.model and self.local_fallback and self.local_fallback.local:
                    processing_model = self.local_fallback
                    evidence, transmission = self._evidence_payload(tenant, owner, evidence_ids, purpose, processing_model)
                    output = processing_model.propose(request, temperature=0.0, timeout=self.policy.timeout_seconds)
                else: raise
            if output.get("schema_version") != self.schema_version or not isinstance(output.get("proposals"), list): raise ValueError("invalid structured model output")
            for raw in output["proposals"][: self.policy.maximum_accepted_memories + self.policy.maximum_associations]:
                proposal = MemoryProposal(**raw); proposal.validate_shape()
                try: accepted.extend(self._apply(proposal, tenant, owner, evidence))
                except Exception as exc: self._quarantine(tenant, proposal, str(exc))
        except Exception as exc:
            validation = "invalid:" + str(exc); output = output or {"error": str(exc)}
        latency = (time.monotonic() - started) * 1000
        with self.store.db:
            self.store.db.execute("INSERT INTO model_runs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", ("run-" + uuid.uuid4().hex, tenant,
                processing_model.provider, processing_model.model, self.prompt_version, self.schema_version, json.dumps(evidence_ids), json.dumps(output), latency,
                sum(len(item["content"]) for item in evidence) // 4, len(json.dumps(output)) // 4, validation, json.dumps(accepted), json.dumps(transmission), cache_key, int(time.time() * 1000)))
        budget["accepted"] += len(accepted)
        return {"status": validation, "accepted": accepted, "transmission": transmission}

    def _apply(self, proposal: MemoryProposal, tenant: str, owner: str, evidence: list[dict[str, Any]]) -> list[str]:
        known = {item["id"]: item["content"] for item in evidence}
        if any(identifier not in known for identifier in proposal.evidence_ids): raise ValueError("proposal references unavailable evidence")
        for identifier, quote in proposal.quoted_spans.items():
            if identifier not in known or quote not in known[identifier]: raise ValueError("supporting quote was not found verbatim")
        if proposal.confidence < 0.55: raise ValueError("low-confidence proposal")
        if proposal.sensitivity in SENSITIVE_CLASSES: raise ValueError("sensitive proposal requires confirmation")
        if re.search(r"(?i)\b(ignore previous|system prompt|agent identity|api key|password)\b", proposal.statement): raise ValueError("identity, policy, or credential proposal")
        # Exact active/superseded deduplication precedes writes.
        duplicate = self.store.db.execute("SELECT record FROM memories WHERE tenant=? AND owner=? AND lower(subject)=lower(?) AND lower(statement)=lower(?) AND status IN ('active','superseded')", (tenant, owner, proposal.subject, proposal.statement)).fetchone()
        if duplicate:
            identifier = json.loads(duplicate[0])["id"]
            for evidence_id in proposal.evidence_ids: self.store.reinforce(identifier, evidence_id, tenant, owner)
            return [identifier]
        if proposal.operation in {"create", "remind"}:
            namespace = MemoryNamespace.PROSPECTIVE if proposal.operation == "remind" else MemoryNamespace(proposal.namespace)
            record = MemoryRecord(tenant, owner, namespace, proposal.subject, proposal.statement, Visibility.PRIVATE,
                proposal.sensitivity, ["conversation", "coding", "planning", "research"], proposal.evidence_ids,
                confidence=proposal.confidence, importance=proposal.importance, provenance="inferred")
            return [self.store.remember(record)["id"]]
        if proposal.operation == "reinforce":
            for evidence_id in proposal.evidence_ids: self.store.reinforce(proposal.target_id, evidence_id, tenant, owner)
            return [proposal.target_id]
        if proposal.operation == "delete": raise ValueError("model deletions require explicit authorization")
        if proposal.operation in {"associate", "supersede", "dispute"}:
            relation = proposal.relation or {"supersede": "supersedes", "dispute": "contradicts"}.get(proposal.operation, "supports")
            return [self.store.associate(proposal.subject, proposal.target_id, relation, tenant, owner)]
        return []

    def _quarantine(self, tenant: str, proposal: MemoryProposal, reason: str) -> str:
        identifier = "quarantine-" + uuid.uuid4().hex
        with self.store.db:
            self.store.db.execute("INSERT INTO quarantined VALUES(?,?,?,?,NULL)", (identifier, tenant, reason, json.dumps(asdict(proposal), sort_keys=True), int(time.time() * 1000)))
        return identifier

    def consolidate(self, *, tenant: str, owner: str, minimum_episodes: int = 3) -> list[str]:
        """Deterministically consolidate repeated episodes while preserving every source memory."""
        rows = self.store.db.execute("SELECT record FROM memories WHERE tenant=? AND owner=? AND namespace='episodic' AND status='active'", (tenant, owner)).fetchall()
        groups: dict[str, list[dict[str, Any]]] = {}
        for row in rows:
            item = json.loads(row[0]); groups.setdefault(item["subject"].strip().lower(), []).append(item)
        created = []
        for items in groups.values():
            if len(items) < minimum_episodes: continue
            evidence_ids = list(dict.fromkeys(identifier for item in items for identifier in item["evidence_ids"]))
            record = MemoryRecord(tenant, owner, MemoryNamespace.SEMANTIC, items[0]["subject"], items[-1]["statement"],
                Visibility.PRIVATE, items[-1]["sensitivity"], items[-1]["allowed_purposes"], evidence_ids,
                confidence=min(0.95, max(item["confidence"] for item in items) + 0.05), importance=max(item["importance"] for item in items), provenance="consolidated", reinforcement=len(items), decay_policy="durable")
            result = self.store.remember(record); created.append(result["id"])
            for item in items: self.store.associate(result["id"], item["id"], "supports", tenant, owner, 1.0)
        return created

    def shutdown(self) -> None:
        self.stop_event.set(); self.worker.join(timeout=2)
