from __future__ import annotations

from dataclasses import asdict, dataclass, field
import json
from pathlib import Path
import threading
import time
import uuid
from typing import Any


@dataclass
class RagTrace:
    trace_id: str = field(default_factory=lambda: uuid.uuid4().hex)
    operation: str = ""
    tenant_id: str = ""
    started_at_ms: int = field(default_factory=lambda: int(time.time() * 1000))
    duration_ms: float = 0.0
    query: str = ""
    provider: str = ""
    model: str = ""
    embedding_model_key: str = ""
    approximate: bool = False
    retrieved_chunk_ids: list[str] = field(default_factory=list)
    published_version_ids: list[str] = field(default_factory=list)
    citation_chunk_ids: list[str] = field(default_factory=list)
    injection_patterns: list[str] = field(default_factory=list)
    status: str = "ok"
    error: str = ""
    attributes: dict[str, Any] = field(default_factory=dict)


class TraceStore:
    def __init__(self, path: str = "", max_memory: int = 10000) -> None:
        self.path = Path(path) if path else None
        self.max_memory = max_memory
        self._items: list[RagTrace] = []
        self._lock = threading.Lock()
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)

    def append(self, trace: RagTrace) -> None:
        encoded = json.dumps(asdict(trace), separators=(",", ":"), ensure_ascii=False)
        with self._lock:
            self._items.append(trace)
            if len(self._items) > self.max_memory:
                del self._items[:len(self._items) - self.max_memory]
            if self.path:
                with self.path.open("a", encoding="utf-8") as handle:
                    handle.write(encoded + "\n")

    def get(self, trace_id: str) -> RagTrace | None:
        with self._lock:
            return next((item for item in reversed(self._items) if item.trace_id == trace_id), None)

    def recent(self, limit: int = 100) -> list[RagTrace]:
        with self._lock:
            return list(reversed(self._items[-limit:]))
