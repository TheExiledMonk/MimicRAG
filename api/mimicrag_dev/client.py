from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request
from typing import Any


class MimicRagError(RuntimeError):
    def __init__(self, message: str, status: int = 0):
        super().__init__(message); self.status = status


class Client:
    def __init__(self, base_url: str = "http://127.0.0.1:8080", api_key: str = "", timeout: float = 30):
        self.base_url = base_url.rstrip("/"); self.api_key = api_key; self.timeout = timeout

    def request(self, method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
        headers = {"Accept": "application/json", "Content-Type": "application/json"}
        if self.api_key: headers["Authorization"] = "Bearer " + self.api_key
        request = urllib.request.Request(self.base_url + path,
            data=None if payload is None else json.dumps(payload).encode(), headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response: return json.load(response)
        except urllib.error.HTTPError as exc:
            try: message = json.load(exc).get("error", str(exc))
            except Exception: message = str(exc)
            raise MimicRagError(message, exc.code) from exc

    def ready(self): return self.request("GET", "/ready")
    def health(self): return self.request("GET", "/health")
    def ingest(self, text: str, source_uri: str, **options): return self.request("POST", "/v1/documents", {"text": text, "source_uri": source_uri, **options})
    def retrieve(self, query: str, **options): return self.request("POST", "/v1/retrieve", {"query": query, **options})
    def answer(self, query: str, **options): return self.request("POST", "/v1/answers", {"query": query, **options})
    def expand(self, node_id: str, **options): return self.request("POST", "/v1/graph/expand", {"node_id": node_id, **options})
    def trace(self, trace_id: str): return self.request("GET", "/v1/traces/" + urllib.parse.quote(trace_id, safe=""))
    def traces(self, limit: int = 100): return self.request("GET", "/v1/traces?limit=" + str(min(max(limit, 1), 1000)))
    def storage(self): return self.request("GET", "/v1/storage")
    def feedback(self, chunk_id: str, relevant: bool, **options): return self.request("POST", "/v1/feedback", {"chunk_id": chunk_id, "relevant": relevant, **options})
    def delete(self, document_id: str, tenant_id: str = "default"): return self.request("DELETE", "/v1/documents/" + urllib.parse.quote(document_id, safe=""), {"tenant_id": tenant_id})
    def append_evidence(self, kind: str, content: str, **options): return self.request("POST", "/v1/evidence", {"kind": kind, "content": content, **options})
    def inspect_evidence(self, evidence_id: str, **options): return self.request("POST", "/v1/evidence/inspect", {"evidence_id": evidence_id, **options})
    def remember(self, subject: str, statement: str, evidence_ids: list[str], **options): return self.request("POST", "/v1/memory/remember", {"subject": subject, "statement": statement, "evidence_ids": evidence_ids, **options})
    def recall_memory(self, query: str, **options): return self.request("POST", "/v1/memory/recall", {"query": query, **options})
    def inspect_memory(self, memory_id: str, **options): return self.request("POST", "/v1/memory/inspect", {"memory_id": memory_id, **options})
    def correct_memory(self, memory_id: str, statement: str, evidence_ids: list[str], **options): return self.request("POST", "/v1/memory/correct", {"memory_id": memory_id, "statement": statement, "evidence_ids": evidence_ids, **options})
    def confirm_memory(self, memory_id: str, **options): return self.request("POST", "/v1/memory/confirm", {"memory_id": memory_id, **options})
    def reject_memory(self, memory_id: str, reason: str = "operator rejection", **options): return self.request("POST", "/v1/memory/reject", {"memory_id": memory_id, "reason": reason, **options})
    def dispute_memory(self, memory_id: str, target_memory_id: str, evidence_id: str, **options): return self.request("POST", "/v1/memory/dispute", {"memory_id": memory_id, "target_memory_id": target_memory_id, "evidence_id": evidence_id, **options})
    def due_memories(self, context: str = "", **options): return self.request("POST", "/v1/memory/due", {"context": context, **options})
    def forget_memory(self, memory_id: str, **options): return self.request("DELETE", "/v1/memory/" + urllib.parse.quote(memory_id, safe=""), options)
    def review_memories(self, **options): return self.request("POST", "/v1/memory/review", options)
    def export_memories(self, **options): return self.request("POST", "/v1/memory/export", options)
    def retrieve_combined(self, query: str, **options): return self.request("POST", "/v1/retrieve/combined", {"query": query, **options})
    def run_dream(self, mode: str = "light", **options): return self.request("POST", "/v1/dream/run", {"mode": mode, "enabled": True, **options})
    def review_refinements(self, **options): return self.request("POST", "/v1/dream/review", options)
    def act_on_refinement(self, refinement_id: str, decision: str, **options): return self.request("POST", "/v1/dream/action", {"refinement_id": refinement_id, "decision": decision, **options})
    def refined_procedure(self, memory_id: str, **options): return self.request("POST", "/v1/dream/procedure", {"memory_id": memory_id, **options})
