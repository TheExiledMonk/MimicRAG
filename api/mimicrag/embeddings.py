from __future__ import annotations

from dataclasses import dataclass
import hashlib
import time

from .models import EmbeddingRecord
from .providers import ModelProvider
from .store import RagStore


def model_key(provider: ModelProvider) -> str:
    config = getattr(provider, "config", None)
    identity = f"{getattr(config, 'provider', type(provider).__name__)}\0{getattr(config, 'base_url', '')}\0{getattr(config, 'model', '')}"
    return hashlib.sha256(identity.encode()).hexdigest()[:24]


@dataclass(frozen=True)
class EmbeddingJobResult:
    considered: int
    embedded: int
    skipped: int
    batches: int
    model_key: str


class EmbeddingIndexer:
    def __init__(self, store: RagStore, provider: ModelProvider, batch_size: int = 64) -> None:
        if batch_size < 1:
            raise ValueError("batch_size must be positive")
        self.store = store
        self.provider = provider
        self.batch_size = batch_size
        self.model_key = model_key(provider)

    def index_tenant(self, tenant_id: str) -> EmbeddingJobResult:
        chunks = self.store.visible_chunks(tenant_id, access_scope=None)
        existing = self.store.embedded_chunk_ids(self.model_key)
        pending = [chunk for chunk in chunks if chunk.chunk_id not in existing]
        stored = 0
        batches = 0
        for offset in range(0, len(pending), self.batch_size):
            batch = pending[offset:offset + self.batch_size]
            vectors = self.provider.embed([chunk.text for chunk in batch])
            if len(vectors) != len(batch):
                raise ValueError("embedding provider returned the wrong vector count")
            dimensions = {len(vector) for vector in vectors}
            if len(dimensions) != 1 or not dimensions or next(iter(dimensions)) == 0:
                raise ValueError("embedding vectors must have one non-zero dimension")
            now = int(time.time() * 1000)
            records = [EmbeddingRecord(chunk.chunk_id, chunk.version_id, chunk.tenant_id, str(chunk.metadata.get("access_scope", "public")), self.model_key, tuple(float(v) for v in vector), now) for chunk, vector in zip(batch, vectors)]
            stored += self.store.put_embeddings(records)
            batches += 1
        return EmbeddingJobResult(len(chunks), stored, len(chunks) - len(pending), batches, self.model_key)
