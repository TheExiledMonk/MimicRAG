from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

from .embeddings import model_key
from .lexical import BM25Index
from .models import RetrievalHit
from .providers import ModelProvider
from .store import RagStore


Reranker = Callable[[str, list[RetrievalHit]], list[RetrievalHit]]


@dataclass(frozen=True)
class RetrievalConfig:
    top_k: int = 10
    candidate_multiplier: int = 4
    rrf_k: int = 60
    vector_weight: float = 1.0
    lexical_weight: float = 1.0
    approximate_threshold: int = 20000
    probes: int = 0


class HybridRetriever:
    def __init__(self, store: RagStore, embedding_provider: ModelProvider, config: RetrievalConfig | None = None, reranker: Reranker | None = None) -> None:
        self.store = store
        self.provider = embedding_provider
        self.config = config or RetrievalConfig()
        self.reranker = reranker
        self.model_key = model_key(embedding_provider)
        self._lexical_cache: dict[str, tuple[tuple[str, ...], BM25Index]] = {}

    def search(self, query: str, tenant_id: str, top_k: int | None = None, access_scope: str = "public") -> list[RetrievalHit]:
        limit = top_k or self.config.top_k
        chunks = self.store.visible_chunks(tenant_id, access_scope)
        if not chunks or limit < 1:
            return []
        by_id = {chunk.chunk_id: chunk for chunk in chunks}
        candidates = max(limit, limit * self.config.candidate_multiplier)
        fingerprint = tuple(chunk.chunk_id for chunk in chunks)
        cached = self._lexical_cache.get(tenant_id)
        if cached is None or cached[0] != fingerprint:
            cached = (fingerprint, BM25Index(chunks))
            self._lexical_cache[tenant_id] = cached
        lexical = cached[1].search(query, candidates)
        vectors = self.provider.embed([query])
        if len(vectors) != 1:
            raise ValueError("embedding provider returned the wrong query vector count")
        approximate = len(chunks) >= self.config.approximate_threshold
        vector = self.store.vector_candidates(vectors[0], tenant_id, access_scope, self.model_key, candidates * 2, approximate, self.config.probes)
        vector = [(chunk_id, score) for chunk_id, score in vector if chunk_id in by_id][:candidates]
        ranks: dict[str, dict[str, int]] = {}
        scores: dict[str, float] = {}
        for name, weight, values in (("vector", self.config.vector_weight, vector), ("lexical", self.config.lexical_weight, lexical)):
            for rank, (chunk_id, _) in enumerate(values, 1):
                ranks.setdefault(chunk_id, {})[name] = rank
                scores[chunk_id] = scores.get(chunk_id, 0.0) + weight / (self.config.rrf_k + rank)
        hits = [RetrievalHit(by_id[chunk_id], score, ranks[chunk_id].get("vector"), ranks[chunk_id].get("lexical")) for chunk_id, score in scores.items() if chunk_id in by_id]
        hits.sort(key=lambda hit: (-hit.score, hit.chunk.chunk_id))
        hits = hits[:candidates]
        if self.reranker:
            hits = self.reranker(query, hits)
        return hits[:limit]
