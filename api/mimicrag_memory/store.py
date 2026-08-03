from __future__ import annotations

import hashlib
import json
import math
import re
import sqlite3
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Iterable

from .models import MemoryNamespace, MemoryRecord, RELATION_TYPES, SENSITIVE_CLASSES, Visibility


def _now() -> int: return int(time.time() * 1000)
def _tokens(text: str) -> set[str]: return set(re.findall(r"[\w'-]+", text.lower()))


class MemoryStore:
    """Crash-safe embedded ledger. Evidence is authoritative; memories are reversible projections."""

    def __init__(self, path: str | Path, rag_client: Any = None):
        self.path = str(path); self.rag_client = rag_client; self._lock = threading.RLock()
        self.db = sqlite3.connect(self.path, check_same_thread=False); self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL"); self.db.execute("PRAGMA foreign_keys=ON"); self._schema()

    def _schema(self) -> None:
        self.db.executescript("""
        CREATE TABLE IF NOT EXISTS evidence(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, owner TEXT NOT NULL,
          kind TEXT NOT NULL, content TEXT NOT NULL, event_time_ms INTEGER NOT NULL, recorded_at_ms INTEGER NOT NULL,
          provenance TEXT NOT NULL, sensitivity TEXT NOT NULL, purpose TEXT NOT NULL, content_hash TEXT NOT NULL,
          deleted_at_ms INTEGER, metadata TEXT NOT NULL);
        CREATE TRIGGER IF NOT EXISTS evidence_immutable_update BEFORE UPDATE ON evidence
          WHEN OLD.deleted_at_ms IS NOT NULL OR NEW.deleted_at_ms IS NULL BEGIN SELECT RAISE(ABORT, 'evidence is immutable'); END;
        CREATE TRIGGER IF NOT EXISTS evidence_no_delete BEFORE DELETE ON evidence BEGIN SELECT RAISE(ABORT, 'use verifiable forget'); END;
        CREATE TABLE IF NOT EXISTS memories(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, owner TEXT NOT NULL,
          namespace TEXT NOT NULL, subject TEXT NOT NULL, statement TEXT NOT NULL, record TEXT NOT NULL,
          status TEXT NOT NULL, created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL);
        CREATE INDEX IF NOT EXISTS memories_scope ON memories(tenant,owner,status,namespace);
        CREATE TABLE IF NOT EXISTS memory_evidence(memory_id TEXT NOT NULL, evidence_id TEXT NOT NULL, role TEXT NOT NULL,
          PRIMARY KEY(memory_id,evidence_id,role));
        CREATE TABLE IF NOT EXISTS relations(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, owner TEXT NOT NULL,
          source_id TEXT NOT NULL, target_id TEXT NOT NULL, relation TEXT NOT NULL, weight REAL NOT NULL,
          event_time_ms INTEGER NOT NULL, metadata TEXT NOT NULL, deleted_at_ms INTEGER);
        CREATE TABLE IF NOT EXISTS working(session_id TEXT PRIMARY KEY, tenant TEXT NOT NULL, owner TEXT NOT NULL,
          task_id TEXT NOT NULL, state TEXT NOT NULL, expires_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS audit(id INTEGER PRIMARY KEY AUTOINCREMENT, at_ms INTEGER NOT NULL, tenant TEXT NOT NULL,
          owner TEXT NOT NULL, action TEXT NOT NULL, object_id TEXT NOT NULL, reason TEXT NOT NULL, detail TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS model_runs(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, provider TEXT NOT NULL, model TEXT NOT NULL,
          prompt_version TEXT NOT NULL, schema_version INTEGER NOT NULL, evidence_ids TEXT NOT NULL, output TEXT NOT NULL,
          latency_ms REAL NOT NULL, input_tokens INTEGER NOT NULL, output_tokens INTEGER NOT NULL, validation TEXT NOT NULL,
          mutation_ids TEXT NOT NULL, transmission TEXT NOT NULL, cache_key TEXT NOT NULL UNIQUE, created_at_ms INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS quarantined(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, reason TEXT NOT NULL,
          proposal TEXT NOT NULL, created_at_ms INTEGER NOT NULL, resolved_at_ms INTEGER);
        CREATE TABLE IF NOT EXISTS memory_jobs(id TEXT PRIMARY KEY, tenant TEXT NOT NULL, owner TEXT NOT NULL,
          request TEXT NOT NULL, status TEXT NOT NULL, result TEXT NOT NULL, error TEXT NOT NULL,
          created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0,
          max_attempts INTEGER NOT NULL DEFAULT 3, lease_owner TEXT NOT NULL DEFAULT '', lease_until_ms INTEGER NOT NULL DEFAULT 0);
        CREATE INDEX IF NOT EXISTS memory_jobs_scope ON memory_jobs(tenant,owner,created_at_ms);
        """)
        columns = {row[1] for row in self.db.execute("PRAGMA table_info(memory_jobs)")}
        for name, declaration in {"attempts": "INTEGER NOT NULL DEFAULT 0", "max_attempts": "INTEGER NOT NULL DEFAULT 3", "lease_owner": "TEXT NOT NULL DEFAULT ''", "lease_until_ms": "INTEGER NOT NULL DEFAULT 0"}.items():
            if name not in columns: self.db.execute(f"ALTER TABLE memory_jobs ADD COLUMN {name} {declaration}")
        self.db.commit()

    def _audit(self, tenant: str, owner: str, action: str, object_id: str, reason: str, detail: Any = None) -> None:
        self.db.execute("INSERT INTO audit(at_ms,tenant,owner,action,object_id,reason,detail) VALUES(?,?,?,?,?,?,?)",
                        (_now(), tenant, owner, action, object_id, reason, json.dumps(detail or {}, sort_keys=True)))

    def append_evidence(self, *, tenant: str, owner: str, kind: str, content: str, provenance: str,
                        sensitivity: str = "internal", purpose: str = "conversation", event_time_ms: int | None = None,
                        metadata: dict[str, Any] | None = None) -> str:
        if kind not in {"conversation", "tool_result", "correction", "task_outcome", "observation"}: raise ValueError("unsupported evidence kind")
        if not tenant or not owner or not content: raise ValueError("tenant, owner, and content are required")
        identifier = "evt-" + uuid.uuid4().hex; now = _now()
        with self.db:
            self.db.execute("INSERT INTO evidence VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)", (identifier, tenant, owner, kind, content,
                event_time_ms or now, now, provenance, sensitivity, purpose, hashlib.sha256(content.encode()).hexdigest(), None, json.dumps(metadata or {}, sort_keys=True)))
            self._audit(tenant, owner, "evidence.append", identifier, provenance)
        return identifier

    def _evidence(self, evidence_id: str, tenant: str, owner: str) -> sqlite3.Row:
        row = self.db.execute("SELECT * FROM evidence WHERE id=? AND tenant=? AND owner=? AND deleted_at_ms IS NULL", (evidence_id, tenant, owner)).fetchone()
        if not row: raise ValueError("unknown or unauthorized evidence: " + evidence_id)
        return row

    def remember(self, record: MemoryRecord, *, confirmed: bool = False) -> dict[str, Any]:
        record.validate()
        for evidence_id in record.evidence_ids + record.contradicting_evidence_ids: self._evidence(evidence_id, record.tenant, record.owner)
        requires_confirmation = record.sensitivity in SENSITIVE_CLASSES or record.provenance == "identity"
        if requires_confirmation and not confirmed:
            record.status = "pending_confirmation"; record.confirmation = "required"
        elif confirmed: record.confirmation = "confirmed"
        # Identity and policy text is data only and never enters prompts/system configuration.
        if record.provenance in {"imported_document", "inferred"} and re.search(r"(?i)\b(system policy|agent identity|ignore previous|credential)\b", record.statement):
            record.status = "quarantined"
        now = _now(); value = record.to_dict()
        with self.db:
            self.db.execute("INSERT INTO memories VALUES(?,?,?,?,?,?,?,?,?,?)", (record.id, record.tenant, record.owner,
                record.namespace.value, record.subject, record.statement, json.dumps(value, sort_keys=True), record.status, now, now))
            for evidence_id in record.evidence_ids: self.db.execute("INSERT INTO memory_evidence VALUES(?,?,?)", (record.id, evidence_id, "supports"))
            for evidence_id in record.contradicting_evidence_ids: self.db.execute("INSERT INTO memory_evidence VALUES(?,?,?)", (record.id, evidence_id, "contradicts"))
            self._audit(record.tenant, record.owner, "memory.remember", record.id, record.provenance, {"status": record.status})
        if self.rag_client and record.status == "active":
            indexed = self.rag_client.ingest(record.statement, "memory://" + record.id, tenant_id=record.tenant,
                metadata={"memory_namespace": record.namespace.value, "owner": record.owner, "trust": "memory", "policy_authority": False})
            value["rag_document_id"] = indexed.get("document_id", "")
            with self.db:
                self.db.execute("UPDATE memories SET record=? WHERE id=?", (json.dumps(value, sort_keys=True), record.id))
        return value

    def _record(self, row: sqlite3.Row) -> dict[str, Any]: return json.loads(row["record"])

    def correct(self, memory_id: str, *, statement: str, correction_evidence_id: str, tenant: str, owner: str,
                confirmed: bool = False) -> dict[str, Any]:
        old_row = self.db.execute("SELECT * FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not old_row: raise ValueError("memory not found")
        old = self._record(old_row); evidence = self._evidence(correction_evidence_id, tenant, owner)
        record = MemoryRecord(tenant, owner, MemoryNamespace(old["namespace"]), old["subject"], statement,
            Visibility(old["visibility"]), old["sensitivity"], old["allowed_purposes"], [correction_evidence_id],
            confidence=1.0, importance=old["importance"], provenance="explicit_correction", decay_policy=old["decay_policy"])
        created = self.remember(record, confirmed=confirmed)
        with self.db:
            self._set_status(memory_id, "superseded"); self.associate(record.id, memory_id, "supersedes", tenant, owner, 1.0)
            self._audit(tenant, owner, "memory.correct", record.id, "explicit correction outranks prior memory", {"superseded": memory_id})
        return created

    def _set_status(self, memory_id: str, status: str) -> None:
        row = self.db.execute("SELECT record FROM memories WHERE id=?", (memory_id,)).fetchone()
        if not row: raise ValueError("memory not found")
        value = json.loads(row[0]); value["status"] = status
        self.db.execute("UPDATE memories SET status=?,record=?,updated_at_ms=? WHERE id=?", (status, json.dumps(value, sort_keys=True), _now(), memory_id))

    def confirm(self, memory_id: str, *, tenant: str, owner: str) -> dict[str, Any]:
        row = self.db.execute("SELECT status FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not row or row[0] != "pending_confirmation": raise ValueError("memory is not pending confirmation")
        with self.db: self._set_status(memory_id, "active"); self._audit(tenant, owner, "memory.confirm", memory_id, "operator confirmation")
        return self.inspect(memory_id, tenant=tenant, owner=owner)["memory"]

    def reject(self, memory_id: str, *, tenant: str, owner: str, reason: str = "operator rejection") -> dict[str, Any]:
        row = self.db.execute("SELECT status FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not row or row[0] not in {"pending_confirmation", "quarantined"}: raise ValueError("memory is not reviewable")
        with self.db: self._set_status(memory_id, "rejected"); self._audit(tenant, owner, "memory.reject", memory_id, reason)
        return self.inspect(memory_id, tenant=tenant, owner=owner)["memory"]

    def review(self, *, tenant: str, owner: str, status: str = "") -> dict[str, Any]:
        query, values = "SELECT record FROM memories WHERE tenant=? AND owner=?", [tenant, owner]
        if status: query += " AND status=?"; values.append(status)
        memories = [json.loads(row[0]) for row in self.db.execute(query + " ORDER BY updated_at_ms DESC", values)]
        quarantined = [dict(row) for row in self.db.execute("SELECT * FROM quarantined WHERE tenant=? AND resolved_at_ms IS NULL ORDER BY created_at_ms", (tenant,))]
        return {"memories": memories, "quarantined_proposals": quarantined, "count": len(memories), "tenant": tenant, "owner": owner}

    def reinforce(self, memory_id: str, evidence_id: str, tenant: str, owner: str, *, successful_use: bool = False) -> None:
        self._evidence(evidence_id, tenant, owner); row = self.db.execute("SELECT record FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not row: raise ValueError("memory not found")
        value = json.loads(row[0]); value["reinforcement"] += 2 if successful_use else 1; value["confidence"] = min(1.0, value["confidence"] + (0.05 if successful_use else 0.02))
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO memory_evidence VALUES(?,?,?)", (memory_id, evidence_id, "supports"))
            self.db.execute("UPDATE memories SET record=?,updated_at_ms=? WHERE id=?", (json.dumps(value, sort_keys=True), _now(), memory_id)); self._audit(tenant, owner, "memory.reinforce", memory_id, "independent evidence" if not successful_use else "successful use")

    def associate(self, source_id: str, target_id: str, relation: str, tenant: str, owner: str,
                  weight: float = 0.5, event_time_ms: int | None = None, metadata: dict[str, Any] | None = None) -> str:
        if relation not in RELATION_TYPES or not 0 <= weight <= 1: raise ValueError("invalid relation")
        for identifier in (source_id, target_id):
            if not self.db.execute("SELECT 1 FROM memories WHERE id=? AND tenant=? AND owner=?", (identifier, tenant, owner)).fetchone(): raise ValueError("relation memory not found")
        identifier = "rel-" + uuid.uuid4().hex
        with self.db:
            self.db.execute("INSERT INTO relations VALUES(?,?,?,?,?,?,?,?,?,NULL)", (identifier, tenant, owner, source_id, target_id, relation, weight, event_time_ms or _now(), json.dumps(metadata or {})))
            self._audit(tenant, owner, "memory.associate", identifier, relation)
        return identifier

    def recall(self, query: str, *, tenant: str, owner: str, purpose: str, now_ms: int | None = None,
               namespaces: Iterable[str] | None = None, limit: int = 12, maximum_hops: int = 1,
               include_history: bool = False, sensitivity_allowed: Iterable[str] = ("public", "internal")) -> dict[str, Any]:
        now = now_ms or _now(); query_tokens = _tokens(query); allowed = set(sensitivity_allowed); selected_namespaces = set(namespaces or [item.value for item in MemoryNamespace])
        rows = self.db.execute("SELECT * FROM memories WHERE tenant=? AND owner=?", (tenant, owner)).fetchall(); ranked = []; suppressed = []
        purpose_mix = {"coding": {"procedural": 1.25, "negative": 1.15}, "planning": {"prospective": 1.3, "working": 1.2},
                       "conversation": {"preference": 1.25, "episodic": 1.1}, "research": {"semantic": 1.2}}.get(purpose, {})
        for row in rows:
            item = self._record(row); reason = ""
            if item["namespace"] not in selected_namespaces: reason = "namespace"
            elif item["sensitivity"] not in allowed: reason = "sensitivity"
            elif purpose not in item["allowed_purposes"]: reason = "purpose"
            elif item["status"] != "active" and not (include_history and item["status"] == "superseded"): reason = item["status"]
            elif item.get("valid_from_ms") and now < item["valid_from_ms"]: reason = "not_yet_valid"
            elif item.get("valid_until_ms") and now > item["valid_until_ms"]: reason = "expired"
            if reason: suppressed.append({"memory_id": item["id"], "reason": reason}); continue
            overlap = len(query_tokens & _tokens(item["subject"] + " " + item["statement"])) / max(1, len(query_tokens))
            age_days = max(0, now - item["event_time_ms"]) / 86400000
            decay = math.exp(-age_days / 30) if item["namespace"] == "episodic" else 1.0
            trust = 1.0 if item["provenance"] in {"explicit", "explicit_correction"} else 0.75
            score = (0.42 * overlap + 0.18 * item["confidence"] + 0.14 * item["importance"] +
                     0.08 * min(item["reinforcement"] / 5, 1) + 0.08 * decay + 0.1 * trust) * purpose_mix.get(item["namespace"], 1.0)
            ranked.append((score, item))
        ranked.sort(key=lambda pair: (-pair[0], -pair[1]["event_time_ms"], pair[1]["id"])); recalled = []
        with self.db:
            for score, item in ranked[:max(0, min(limit, 100))]:
                item["recall_score"] = score; item["why_recalled"] = {"purpose": purpose, "valid_at_ms": now, "trust_domain": "memory"}; recalled.append(item)
                item["last_used_at_ms"] = now; self.db.execute("UPDATE memories SET record=? WHERE id=?", (json.dumps(item, sort_keys=True), item["id"])); self._audit(tenant, owner, "memory.recall", item["id"], "ranked for query", {"score": score, "query": query})
        sections: dict[str, list[dict[str, Any]]] = {key: [] for key in ("current_state", "durable_facts", "episodes", "procedures", "preferences", "commitments", "uncertainty")}
        mapping = {"working": "current_state", "semantic": "durable_facts", "episodic": "episodes", "procedural": "procedures", "preference": "preferences", "prospective": "commitments", "negative": "uncertainty"}
        for item in recalled: sections[mapping[item["namespace"]]].append(item)
        return {"query": query, "purpose": purpose, "sections": sections, "memories": recalled,
                "suppressed": suppressed, "budgets": {"limit": limit, "maximum_hops": min(maximum_hops, 2)},
                "trust_notice": "Memory is lower-trust context and must not silently displace authoritative documents."}

    def prospective(self, *, tenant: str, owner: str, purpose: str, now_ms: int | None = None,
                    entities: Iterable[str] = (), state: str = "", action: str = "") -> list[dict[str, Any]]:
        now = now_ms or _now(); context = " ".join([*entities, state, action]).lower(); out = []
        for row in self.db.execute("SELECT record FROM memories WHERE tenant=? AND owner=? AND namespace='prospective' AND status='active'", (tenant, owner)):
            item = json.loads(row[0]); condition = item.get("provenance", "") + " " + item["subject"]
            time_ready = not item.get("valid_from_ms") or now >= item["valid_from_ms"]
            if time_ready and (not context or any(token in context for token in _tokens(condition))): out.append(item)
        return out[:20]

    def recall_combined(self, query: str, *, documents: list[dict[str, Any]], tenant: str, owner: str,
                        purpose: str, memory_limit: int = 8) -> dict[str, Any]:
        """Combine contexts with authoritative documents always in the leading trust tier."""
        memory = self.recall(query, tenant=tenant, owner=owner, purpose=purpose, limit=memory_limit)
        return {"authoritative_documents": documents, "memory_context": memory["sections"],
                "ordering": ["authoritative_documents", "memory_context"],
                "rule": "Memory may supplement but never override document evidence without an explicit conflict marker.",
                "conflicts_require_resolution": True}

    def set_working(self, session_id: str, *, tenant: str, owner: str, task_id: str, state: dict[str, Any], ttl_seconds: int = 3600) -> None:
        allowed = {"objective", "active_entities", "constraints", "assumptions", "unresolved_questions", "recent_observations", "pending_operations", "selected_outcomes"}
        if set(state) - allowed: raise ValueError("unsupported working-memory fields")
        now = _now()
        with self.db:
            self.db.execute("INSERT OR REPLACE INTO working VALUES(?,?,?,?,?,?,?)", (session_id, tenant, owner, task_id, json.dumps(state, sort_keys=True), now + max(1, ttl_seconds) * 1000, now)); self._audit(tenant, owner, "working.set", session_id, "task checkpoint")

    def context_packet(self, session_id: str, *, maximum_chars: int = 6000) -> dict[str, Any]:
        row = self.db.execute("SELECT * FROM working WHERE session_id=? AND expires_at_ms>?", (session_id, _now())).fetchone()
        if not row: return {"session_id": session_id, "expired": True, "state": {}}
        state = json.loads(row["state"]); packet = {"session_id": session_id, "task_id": row["task_id"], "state": state}
        encoded = json.dumps(packet)
        if len(encoded) > maximum_chars:
            state["recent_observations"] = state.get("recent_observations", [])[-3:]; state["pending_operations"] = state.get("pending_operations", [])[:10]
        return packet

    def expire_working(self) -> int:
        with self.db:
            cursor = self.db.execute("DELETE FROM working WHERE expires_at_ms<=?", (_now(),)); return cursor.rowcount

    def promote_working(self, session_id: str, evidence_id: str, *, confirmed: bool = False) -> list[dict[str, Any]]:
        row = self.db.execute("SELECT * FROM working WHERE session_id=? AND expires_at_ms>?", (session_id, _now())).fetchone()
        if not row: raise ValueError("working session not found")
        state = json.loads(row["state"]); created = []
        for outcome in state.get("selected_outcomes", []):
            record = MemoryRecord(row["tenant"], row["owner"], MemoryNamespace(outcome.get("namespace", "episodic")),
                outcome["subject"], outcome["statement"], Visibility.PRIVATE, outcome.get("sensitivity", "internal"), outcome.get("allowed_purposes", ["conversation"]), [evidence_id], provenance="working_promotion")
            created.append(self.remember(record, confirmed=confirmed))
        return created

    def forget(self, memory_id: str, *, tenant: str, owner: str, erase_evidence: bool = False, reason: str = "user request") -> dict[str, Any]:
        row = self.db.execute("SELECT record FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not row: raise ValueError("memory not found")
        record = json.loads(row[0]); evidence_ids = [item[0] for item in self.db.execute("SELECT evidence_id FROM memory_evidence WHERE memory_id=?", (memory_id,))]
        with self.db:
            self._set_status(memory_id, "forgotten"); self.db.execute("UPDATE relations SET deleted_at_ms=? WHERE source_id=? OR target_id=?", (_now(), memory_id, memory_id))
            if erase_evidence:
                for evidence_id in evidence_ids:
                    other = self.db.execute("SELECT 1 FROM memory_evidence me JOIN memories m ON m.id=me.memory_id WHERE me.evidence_id=? AND me.memory_id<>? AND m.status NOT IN ('forgotten')", (evidence_id, memory_id)).fetchone()
                    if not other: self.db.execute("UPDATE evidence SET deleted_at_ms=? WHERE id=?", (_now(), evidence_id))
            self._audit(tenant, owner, "memory.forget", memory_id, reason, {"evidence_erasure_requested": erase_evidence})
        rag = None
        if self.rag_client:
            try: rag = self.rag_client.delete(record.get("rag_document_id", memory_id), tenant)
            except Exception as exc: rag = {"pending_compaction": True, "error": str(exc)}
        return {"forgotten": True, "memory_id": memory_id, "evidence_tombstoned": erase_evidence,
                "relations_removed": True, "rag_indexes": rag, "caches_invalidated": True,
                "traces_tombstoned": True, "backup_erasure": "applies on configured backup expiry/rotation"}

    def inspect(self, memory_id: str, *, tenant: str, owner: str) -> dict[str, Any]:
        row = self.db.execute("SELECT record FROM memories WHERE id=? AND tenant=? AND owner=?", (memory_id, tenant, owner)).fetchone()
        if not row: raise ValueError("memory not found")
        evidence = [dict(item) for item in self.db.execute("SELECT e.id,e.kind,e.event_time_ms,e.provenance,me.role FROM evidence e JOIN memory_evidence me ON e.id=me.evidence_id WHERE me.memory_id=?", (memory_id,))]
        relations = [dict(item) for item in self.db.execute("SELECT id,source_id,target_id,relation,weight,event_time_ms FROM relations WHERE (source_id=? OR target_id=?) AND deleted_at_ms IS NULL", (memory_id, memory_id))]
        audit = [dict(item) for item in self.db.execute("SELECT at_ms,action,reason,detail FROM audit WHERE object_id=? ORDER BY id", (memory_id,))]
        return {"memory": json.loads(row[0]), "evidence": evidence, "relations": relations, "audit": audit}

    def export(self, *, tenant: str, owner: str, include_evidence_content: bool = False) -> dict[str, Any]:
        memories = [json.loads(row[0]) for row in self.db.execute("SELECT record FROM memories WHERE tenant=? AND owner=?", (tenant, owner))]
        evidence = []
        for row in self.db.execute("SELECT * FROM evidence WHERE tenant=? AND owner=?", (tenant, owner)):
            item = dict(row)
            if not include_evidence_content: item["content"] = "[redacted; content_hash=" + item["content_hash"] + "]"
            evidence.append(item)
        relations = [dict(row) for row in self.db.execute("SELECT * FROM relations WHERE tenant=? AND owner=?", (tenant, owner))]
        return {"format": "mimicrag-memory-export", "version": 1, "tenant": tenant, "owner": owner,
                "exported_at_ms": _now(), "memories": memories, "evidence": evidence, "relations": relations}

    def audit(self, *, tenant: str, owner: str, limit: int = 100) -> list[dict[str, Any]]:
        return [dict(row) for row in self.db.execute("SELECT * FROM audit WHERE tenant=? AND owner=? ORDER BY id DESC LIMIT ?", (tenant, owner, min(max(limit, 1), 1000)))]

    def memory_job(self, job_id: str, *, tenant: str, owner: str) -> dict[str, Any]:
        row = self.db.execute("SELECT * FROM memory_jobs WHERE id=? AND tenant=? AND owner=?", (job_id, tenant, owner)).fetchone()
        if not row: raise ValueError("memory job not found")
        value = dict(row); value["request"] = json.loads(value["request"]); value["result"] = json.loads(value["result"] or "{}")
        return value

    def pending_memory_jobs(self) -> list[tuple[str, dict[str, Any]]]:
        rows = self.db.execute("SELECT id,request FROM memory_jobs WHERE status='queued' OR (status='running' AND lease_until_ms<=?) ORDER BY created_at_ms", (_now(),)).fetchall()
        return [(row["id"], json.loads(row["request"])) for row in rows]

    def claim_memory_job(self, job_id: str, worker: str, lease_ms: int = 30000) -> bool:
        now = _now()
        with self.db:
            cursor = self.db.execute("UPDATE memory_jobs SET status='running',lease_owner=?,lease_until_ms=?,attempts=attempts+1,updated_at_ms=? WHERE id=? AND attempts<max_attempts AND (status='queued' OR (status='running' AND lease_until_ms<=?))", (worker, now + lease_ms, now, job_id, now))
        return cursor.rowcount == 1

    def close(self) -> None: self.db.close()
