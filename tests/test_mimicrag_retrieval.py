import hashlib
import unittest

from mimicrag import (
    ChunkingConfig,
    ContextBuilder,
    EmbeddingIndexer,
    HybridRetriever,
    InMemoryRagStore,
    Ingestor,
    RetrievalConfig,
    TextChunker,
)
from mimicrag.config import ModelConfig
from mimicrag.providers import ModelProvider


VOCABULARY = ("python", "database", "volcano", "ocean", "vector", "security", "banana", "satellite")


class DeterministicProvider(ModelProvider):
    capabilities = frozenset({"embedding"})

    def __init__(self):
        self.config = ModelConfig("custom", "test-embedding", base_url="memory://")
        self.calls = 0

    def chat(self, messages, **options):
        return ""

    def embed(self, texts, **options):
        self.calls += 1
        result = []
        for text in texts:
            lowered = text.casefold()
            result.append([float(lowered.count(term)) for term in VOCABULARY])
        return result


class TestMimicRagRetrieval(unittest.TestCase):
    def setUp(self):
        self.store = InMemoryRagStore()
        chunker = TextChunker(ChunkingConfig(target_chars=1000, overlap_chars=0))
        self.ingestor = Ingestor(self.store, chunker)
        self.provider = DeterministicProvider()

    def test_embedding_index_is_batched_and_idempotent(self):
        self.ingestor.ingest(text="python database vector", source_uri="a", tenant_id="one")
        self.ingestor.ingest(text="ocean satellite", source_uri="b", tenant_id="one")
        indexer = EmbeddingIndexer(self.store, self.provider, batch_size=1)
        first = indexer.index_tenant("one")
        second = indexer.index_tenant("one")
        self.assertEqual((first.embedded, first.batches), (2, 2))
        self.assertEqual((second.embedded, second.skipped, second.batches), (0, 2, 0))

    def test_hybrid_retrieval_and_tenant_isolation(self):
        expected = self.ingestor.ingest(text="python vector database indexing", source_uri="good", tenant_id="one")
        self.ingestor.ingest(text="ocean volcano satellite", source_uri="noise", tenant_id="one")
        forbidden = self.ingestor.ingest(text="python vector database exact secret", source_uri="secret", tenant_id="two")
        EmbeddingIndexer(self.store, self.provider).index_tenant("one")
        EmbeddingIndexer(self.store, self.provider).index_tenant("two")
        hits = HybridRetriever(self.store, self.provider).search("python vector index", "one", top_k=2)
        self.assertEqual(hits[0].chunk.version_id, expected.version_id)
        self.assertTrue(all(hit.chunk.version_id != forbidden.version_id for hit in hits))
        self.assertIsNotNone(hits[0].vector_rank)
        self.assertIsNotNone(hits[0].lexical_rank)

    def test_access_scope_is_enforced(self):
        self.ingestor.ingest(text="public python overview", source_uri="public", tenant_id="one")
        secret = self.ingestor.ingest(text="classified python vector details", source_uri="private", tenant_id="one", metadata={"access_scope": "team-red"})
        EmbeddingIndexer(self.store, self.provider).index_tenant("one")
        public_hits = HybridRetriever(self.store, self.provider).search("classified python", "one")
        scoped_hits = HybridRetriever(self.store, self.provider).search("classified python", "one", access_scope="team-red")
        self.assertTrue(all(hit.chunk.version_id != secret.version_id for hit in public_hits))
        self.assertTrue(any(hit.chunk.version_id == secret.version_id for hit in scoped_hits))

    def test_only_current_generation_is_retrieved(self):
        first = self.ingestor.ingest(text="python database", source_uri="same", tenant_id="one")
        second = self.ingestor.ingest(text="ocean satellite", source_uri="same", tenant_id="one")
        EmbeddingIndexer(self.store, self.provider).index_tenant("one")
        hits = HybridRetriever(self.store, self.provider).search("python database", "one")
        self.assertTrue(all(hit.chunk.version_id != first.version_id for hit in hits))
        self.assertTrue(any(hit.chunk.version_id == second.version_id for hit in hits))

    def test_context_budget_deduplication_and_citations(self):
        self.ingestor.ingest(text="python database vector", source_uri="file:///guide", title="Guide", tenant_id="one")
        EmbeddingIndexer(self.store, self.provider).index_tenant("one")
        hit = HybridRetriever(self.store, self.provider).search("python", "one", top_k=1)[0]
        context = ContextBuilder(self.store, token_budget=100).build([hit, hit])
        self.assertEqual(len(context.citations), 1)
        self.assertEqual(context.citations[0].source_uri, "file:///guide")
        self.assertEqual(context.omitted_hits, 1)
        self.assertTrue(context.text.startswith("[1]"))

    def test_retrieval_fixture_recall_at_one(self):
        subjects = ["python", "database", "volcano", "ocean", "vector", "security", "banana", "satellite"]
        expected = {}
        for subject in subjects:
            result = self.ingestor.ingest(text=f"A focused reference about {subject} {subject} principles.", source_uri=subject, tenant_id="eval")
            expected[subject] = result.version_id
        EmbeddingIndexer(self.store, self.provider).index_tenant("eval")
        retriever = HybridRetriever(self.store, self.provider, RetrievalConfig(top_k=1))
        correct = sum(retriever.search(f"explain {subject}", "eval", 1)[0].chunk.version_id == expected[subject] for subject in subjects)
        self.assertEqual(correct / len(subjects), 1.0)


if __name__ == "__main__":
    unittest.main()
