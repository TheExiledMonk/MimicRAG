"""Integrated retrieval-augmented generation primitives for MimicDB."""

from .chunking import ChunkingConfig, TextChunker
from .config import ModelConfig, RagConfig, load_config
from .ingest import IngestionResult, Ingestor
from .context import ContextBuilder, ContextResult, lexical_overlap_reranker, provider_reranker
from .embeddings import EmbeddingIndexer, EmbeddingJobResult, model_key
from .models import Chunk, Citation, Document, DocumentVersion, EmbeddingRecord, Publication, RetrievalHit
from .providers import ModelProvider, ProviderError, create_provider
from .retrieval import HybridRetriever, RetrievalConfig
from .store import InMemoryRagStore, MimicDBRagStore, RagStore, identity_tag, tenant_tag

__all__ = [
    "Chunk",
    "Citation",
    "ChunkingConfig",
    "Document",
    "DocumentVersion",
    "EmbeddingIndexer",
    "EmbeddingJobResult",
    "EmbeddingRecord",
    "ContextBuilder",
    "ContextResult",
    "HybridRetriever",
    "InMemoryRagStore",
    "IngestionResult",
    "Ingestor",
    "MimicDBRagStore",
    "ModelConfig",
    "ModelProvider",
    "ProviderError",
    "Publication",
    "RetrievalConfig",
    "RetrievalHit",
    "RagConfig",
    "RagStore",
    "TextChunker",
    "create_provider",
    "load_config",
    "lexical_overlap_reranker",
    "model_key",
    "provider_reranker",
    "tenant_tag",
    "identity_tag",
]
