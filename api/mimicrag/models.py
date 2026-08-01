from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass(frozen=True)
class Document:
    document_id: str
    tenant_id: str
    source_uri: str
    title: str
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class DocumentVersion:
    version_id: str
    document_id: str
    generation: int
    content_hash: str
    created_at_ms: int
    chunk_count: int


@dataclass(frozen=True)
class Chunk:
    chunk_id: str
    document_id: str
    version_id: str
    tenant_id: str
    ordinal: int
    text: str
    start_char: int
    end_char: int
    token_estimate: int
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class Publication:
    document_id: str
    version_id: str
    generation: int
    published_at_ms: int


@dataclass(frozen=True)
class EmbeddingRecord:
    chunk_id: str
    version_id: str
    tenant_id: str
    access_scope: str
    model_key: str
    vector: tuple[float, ...]
    created_at_ms: int


@dataclass(frozen=True)
class RetrievalHit:
    chunk: Chunk
    score: float
    vector_rank: int | None = None
    lexical_rank: int | None = None


@dataclass(frozen=True)
class Citation:
    citation_id: int
    chunk_id: str
    document_id: str
    source_uri: str
    title: str
    start_char: int
    end_char: int
