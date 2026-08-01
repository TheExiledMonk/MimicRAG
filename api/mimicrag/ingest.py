from __future__ import annotations

from dataclasses import dataclass
import hashlib
import time
import uuid
from typing import Any

from .chunking import TextChunker
from .models import Chunk, Document, DocumentVersion, Publication
from .store import RagStore


@dataclass(frozen=True)
class IngestionResult:
    document_id: str
    version_id: str
    generation: int
    chunk_count: int
    unchanged: bool = False


class Ingestor:
    def __init__(self, store: RagStore, chunker: TextChunker | None = None) -> None:
        self.store = store
        self.chunker = chunker or TextChunker()

    def ingest(self, *, text: str, source_uri: str, tenant_id: str = "default", title: str = "", metadata: dict[str, Any] | None = None, document_id: str = "") -> IngestionResult:
        if not text.strip():
            raise ValueError("document text is empty")
        document_id = document_id or hashlib.sha256(f"{tenant_id}\0{source_uri}".encode()).hexdigest()[:32]
        content_hash = hashlib.sha256(text.encode("utf-8")).hexdigest()
        current = self.store.current_version(document_id)
        if current and current.content_hash == content_hash:
            return IngestionResult(document_id, current.version_id, current.generation, current.chunk_count, True)
        generation = self.store.next_generation(document_id)
        version_id = uuid.uuid4().hex
        slices = self.chunker.split(text)
        document = Document(document_id, tenant_id, source_uri, title, metadata or {})
        chunks = [Chunk(uuid.uuid4().hex, document_id, version_id, tenant_id, ordinal, item.text, item.start_char, item.end_char, item.token_estimate, metadata or {}) for ordinal, item in enumerate(slices)]
        now = int(time.time() * 1000)
        version = DocumentVersion(version_id, document_id, generation, content_hash, now, len(chunks))
        self.store.stage(document, version, chunks)
        # Commit point: readers cannot observe this version before this append.
        self.store.publish(Publication(document_id, version_id, generation, int(time.time() * 1000)))
        return IngestionResult(document_id, version_id, generation, len(chunks))
