from __future__ import annotations

import time
import uuid
from dataclasses import asdict, dataclass, field
from enum import Enum
from typing import Any


class MemoryNamespace(str, Enum):
    WORKING = "working"
    EPISODIC = "episodic"
    SEMANTIC = "semantic"
    PROCEDURAL = "procedural"
    PREFERENCE = "preference"
    PROSPECTIVE = "prospective"
    NEGATIVE = "negative"


class Visibility(str, Enum):
    PRIVATE = "private"
    TASK = "task"
    TEAM = "team"
    TENANT = "tenant"


RELATION_TYPES = {"supports", "contradicts", "supersedes", "caused", "resolved_by", "preference_of", "remind_when"}
SENSITIVE_CLASSES = {"sensitive", "personal", "legal", "financial", "identity"}


@dataclass
class MemoryPolicy:
    remote_processing: bool = False
    allowed_remote_sensitivity: set[str] = field(default_factory=lambda: {"public", "internal"})
    local_only_sensitivity: set[str] = field(default_factory=lambda: {"sensitive", "personal", "legal", "financial", "identity"})
    allowed_purposes: set[str] = field(default_factory=lambda: {"conversation", "coding", "planning", "research"})
    secret_patterns: list[str] = field(default_factory=lambda: [r"(?i)(api[_ -]?key|token|password)\s*[:=]\s*\S+", r"\bsk-[A-Za-z0-9_-]{12,}\b"])
    provider_retention: str = "none"
    residency: str = "unspecified"
    timeout_seconds: float = 30.0
    maximum_content_chars: int = 24000
    maximum_calls_per_session: int = 2
    maximum_tokens_per_session: int = 4000
    maximum_accepted_memories: int = 12
    maximum_associations: int = 24
    maximum_reflection_depth: int = 1


@dataclass
class MemoryRecord:
    tenant: str
    owner: str
    namespace: MemoryNamespace
    subject: str
    statement: str
    visibility: Visibility
    sensitivity: str
    allowed_purposes: list[str]
    evidence_ids: list[str]
    id: str = field(default_factory=lambda: "mem-" + uuid.uuid4().hex)
    schema_version: int = 1
    event_time_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    recorded_at_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    valid_from_ms: int | None = None
    valid_until_ms: int | None = None
    confidence: float = 0.7
    importance: float = 0.5
    provenance: str = "explicit"
    reinforcement: int = 1
    decay_policy: str = "episodic-30d"
    status: str = "active"
    contradicting_evidence_ids: list[str] = field(default_factory=list)
    confirmation: str = "not_required"
    last_used_at_ms: int = 0

    def validate(self) -> None:
        if not self.tenant or not self.owner or not self.subject or not self.statement: raise ValueError("tenant, owner, subject, and statement are required")
        if not self.evidence_ids: raise ValueError("at least one evidence ID is required")
        if not 0 <= self.confidence <= 1 or not 0 <= self.importance <= 1: raise ValueError("confidence and importance must be between 0 and 1")
        if not self.allowed_purposes: raise ValueError("allowed_purposes is required")
        if self.valid_from_ms is not None and self.valid_until_ms is not None and self.valid_until_ms < self.valid_from_ms: raise ValueError("invalid validity interval")

    def to_dict(self) -> dict[str, Any]:
        value = asdict(self); value["namespace"] = self.namespace.value; value["visibility"] = self.visibility.value; return value


@dataclass
class MemoryProposal:
    operation: str
    evidence_ids: list[str]
    subject: str = ""
    statement: str = ""
    namespace: str = "semantic"
    confidence: float = 0.0
    importance: float = 0.5
    target_id: str = ""
    relation: str = ""
    quoted_spans: dict[str, str] = field(default_factory=dict)
    sensitivity: str = "internal"
    schema_version: int = 1

    def validate_shape(self) -> None:
        if self.schema_version != 1: raise ValueError("unsupported proposal schema")
        if self.operation not in {"create", "reinforce", "associate", "supersede", "dispute", "remind", "delete"}: raise ValueError("unsupported proposal operation")
        if not self.evidence_ids: raise ValueError("proposal requires evidence IDs")
        if self.operation in {"create", "remind"} and (not self.subject or not self.statement): raise ValueError("proposal requires subject and statement")
        if not 0 <= self.confidence <= 1: raise ValueError("proposal confidence out of bounds")
        if self.relation and self.relation not in RELATION_TYPES: raise ValueError("unsupported relation")
