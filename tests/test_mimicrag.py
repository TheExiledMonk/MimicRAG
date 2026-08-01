import json
import os
import tempfile
import unittest
from unittest.mock import patch

from mimicrag import InMemoryRagStore, Ingestor, ModelConfig, TextChunker, ChunkingConfig, create_provider, load_config
from mimicrag.providers import AnthropicProvider, AzureOpenAIProvider, OpenAICompatibleProvider


class TestMimicRagRunOne(unittest.TestCase):
    def test_ingestion_publication_and_idempotence(self):
        store = InMemoryRagStore()
        ingestor = Ingestor(store, TextChunker(ChunkingConfig(target_chars=40, overlap_chars=5, min_chars=10)))
        first = ingestor.ingest(text="A useful first paragraph. A useful second paragraph.", source_uri="file:///a", tenant_id="t1")
        self.assertGreater(first.chunk_count, 1)
        self.assertEqual(store.current_version(first.document_id).version_id, first.version_id)
        self.assertEqual(len(store.current_chunks(first.document_id)), first.chunk_count)
        duplicate = ingestor.ingest(text="A useful first paragraph. A useful second paragraph.", source_uri="file:///a", tenant_id="t1")
        self.assertTrue(duplicate.unchanged)
        second = ingestor.ingest(text="Replacement content is now the only visible generation.", source_uri="file:///a", tenant_id="t1")
        self.assertEqual(second.generation, 2)
        self.assertTrue(all(c.version_id == second.version_id for c in store.current_chunks(first.document_id)))

    def test_unpublished_stage_is_invisible(self):
        store = InMemoryRagStore()
        ingestor = Ingestor(store)
        original_publish = store.publish
        store.publish = lambda publication: (_ for _ in ()).throw(RuntimeError("crash"))
        with self.assertRaises(RuntimeError):
            ingestor.ingest(text="not visible", source_uri="file:///crash")
        document_id = next(iter(store.documents)).document_id
        self.assertIsNone(store.current_version(document_id))
        self.assertEqual(store.current_chunks(document_id), [])
        store.publish = original_publish

    def test_provider_selection_and_secret_redaction(self):
        anthropic = create_provider(ModelConfig("anthropic", "claude", api_key="secret"))
        custom = create_provider(ModelConfig("custom", "local", base_url="http://localhost/v1"))
        self.assertIsInstance(anthropic, AnthropicProvider)
        self.assertIsInstance(custom, OpenAICompatibleProvider)
        self.assertNotIn("secret", repr(anthropic.config))
        self.assertEqual(anthropic.config.safe_dict()["api_key"], "***")

    def test_known_compatible_provider_gets_default_url(self):
        raw = {"chat": {"provider": "groq", "model": "chat"}, "embedding": {"provider": "ollama", "model": "embed"}}
        with tempfile.NamedTemporaryFile("w", suffix=".json") as handle:
            json.dump(raw, handle)
            handle.flush()
            config = load_config(handle.name)
        self.assertEqual(config.chat.base_url, "https://api.groq.com/openai/v1")
        self.assertIsInstance(create_provider(config.chat), OpenAICompatibleProvider)

    def test_azure_uses_deployment_endpoint_adapter(self):
        provider = create_provider(ModelConfig("azure_openai", "deployment", api_key="key", base_url="https://example.openai.azure.com/openai/deployments/demo", api_version="test-version"))
        self.assertIsInstance(provider, AzureOpenAIProvider)
        self.assertEqual(provider._path("/embeddings"), "/embeddings?api-version=test-version")
        self.assertEqual(provider._azure_headers(), {"api-key": "key"})

    def test_load_config_requires_selected_environment_secret(self):
        raw = {"chat": {"provider": "openai", "model": "chat", "api_key_env": "TEST_CHAT_KEY"}, "embedding": {"provider": "ollama", "model": "embed"}}
        with tempfile.NamedTemporaryFile("w", suffix=".json") as handle:
            json.dump(raw, handle)
            handle.flush()
            with self.assertRaises(ValueError):
                load_config(handle.name, environ={})
            config = load_config(handle.name, environ={"TEST_CHAT_KEY": "value"})
            self.assertEqual(config.chat.resolved_api_key({"TEST_CHAT_KEY": "value"}), "value")


if __name__ == "__main__":
    unittest.main()
