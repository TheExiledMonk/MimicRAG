from __future__ import annotations

from dataclasses import dataclass

from .models import Citation, RetrievalHit
from .store import RagStore


@dataclass(frozen=True)
class ContextResult:
    text: str
    citations: tuple[Citation, ...]
    token_estimate: int
    omitted_hits: int


class ContextBuilder:
    def __init__(self, store: RagStore, token_budget: int = 4000) -> None:
        self.store = store
        self.token_budget = token_budget

    def build(self, hits: list[RetrievalHit]) -> ContextResult:
        blocks: list[str] = []
        citations: list[Citation] = []
        used: set[str] = set()
        tokens = 0
        omitted = 0
        for hit in hits:
            chunk = hit.chunk
            normalized = " ".join(chunk.text.casefold().split())
            if normalized in used:
                omitted += 1
                continue
            overhead = 12
            if tokens + chunk.token_estimate + overhead > self.token_budget:
                omitted += 1
                continue
            document = self.store.document(chunk.document_id)
            citation_id = len(citations) + 1
            citations.append(Citation(citation_id, chunk.chunk_id, chunk.document_id, document.source_uri if document else "", document.title if document else "", chunk.start_char, chunk.end_char))
            blocks.append(f"[{citation_id}] {chunk.text}")
            used.add(normalized)
            tokens += chunk.token_estimate + overhead
        return ContextResult("\n\n".join(blocks), tuple(citations), tokens, omitted)


def lexical_overlap_reranker(query: str, hits: list[RetrievalHit]) -> list[RetrievalHit]:
    from .lexical import tokenize
    terms = set(tokenize(query))
    return sorted(hits, key=lambda hit: (-(len(terms.intersection(tokenize(hit.chunk.text))) * 0.001 + hit.score), hit.chunk.chunk_id))


def provider_reranker(provider):
    def rerank(query: str, hits: list[RetrievalHit]) -> list[RetrievalHit]:
        rankings = provider.rerank(query, [hit.chunk.text for hit in hits])
        return [hits[index] for index, _ in rankings if 0 <= index < len(hits)]
    return rerank
