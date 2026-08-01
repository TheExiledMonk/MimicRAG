"""Integrated retrieval-augmented generation primitives for MimicDB."""

from .chunking import ChunkingConfig, TextChunker
from .config import ModelConfig, RagConfig, ServerConfig, StorageConfig, load_config
from .evaluation import EvaluationCase, EvaluationResult, evaluate
from .ingest import IngestionResult, Ingestor
from .context import ContextBuilder, ContextResult, lexical_overlap_reranker, provider_reranker
from .embeddings import EmbeddingIndexer, EmbeddingJobResult, model_key
from .models import Chunk, Citation, Document, DocumentVersion, EmbeddingRecord, Publication, RetrievalHit
from .providers import ModelProvider, ProviderError, create_provider
from .retrieval import HybridRetriever, RetrievalConfig
from .runtime import AnswerResult, RagRuntime, create_store
from .security import InjectionAssessment, PolicyError, RateLimitError, SlidingWindowRateLimiter, assess_prompt_injection
from .store import InMemoryRagStore, MimicDBRagStore, RagStore, identity_tag, tenant_tag
from .tracing import RagTrace, TraceStore

__all__ = [
    "Chunk",
    "AnswerResult",
    "Citation",
    "ChunkingConfig",
    "Document",
    "DocumentVersion",
    "EmbeddingIndexer",
    "EmbeddingJobResult",
    "EmbeddingRecord",
    "EvaluationCase",
    "EvaluationResult",
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
    "RagRuntime",
    "RagStore",
    "RagTrace",
    "ServerConfig",
    "StorageConfig",
    "TraceStore",
    "InjectionAssessment",
    "PolicyError",
    "RateLimitError",
    "SlidingWindowRateLimiter",
    "TextChunker",
    "create_provider",
    "create_store",
    "evaluate",
    "load_config",
    "lexical_overlap_reranker",
    "model_key",
    "provider_reranker",
    "tenant_tag",
    "identity_tag",
    "assess_prompt_injection",
]
