import json
import unittest

try:
    from fastapi.testclient import TestClient
except ImportError:
    TestClient = None

from mimicrag import InMemoryRagStore, ModelConfig, RagConfig, RagRuntime, ServerConfig
from mimicrag.providers import ModelProvider


class ServerProvider(ModelProvider):
    def __init__(self):
        self.config = ModelConfig("custom", "server-test", base_url="memory://")
        self.last_messages = []

    def embed(self, texts, **options):
        return [[float(text.casefold().count(word)) for word in ("python", "database", "ocean")] for text in texts]

    def chat(self, messages, **options):
        self.last_messages = messages
        return "Server answer [1]."

    def stream_chat(self, messages, **options):
        yield "Server "
        yield "answer [1]."


@unittest.skipIf(TestClient is None, "FastAPI test dependencies are unavailable")
class TestMimicRagServer(unittest.TestCase):
    def setUp(self):
        from mimicrag.server import create_app
        provider = ServerProvider()
        self.provider = provider
        config = RagConfig(provider.config, provider.config, server=ServerConfig(api_key="test-key", requests_per_minute=1000))
        self.runtime = RagRuntime(config, InMemoryRagStore(), provider, provider)
        self.client = TestClient(create_app(config, self.runtime))
        self.headers = {"Authorization": "Bearer test-key"}

    def tearDown(self):
        self.runtime.close()

    def test_health_auth_ingest_retrieve_answer_and_trace(self):
        self.assertEqual(self.client.get("/health").status_code, 200)
        self.assertEqual(self.client.post("/v1/retrieve", json={"query": "x"}).status_code, 401)
        ingested = self.client.post("/v1/documents", headers=self.headers, json={"text": "python vector database", "source_uri": "file:///guide", "tenant_id": "t", "background": False})
        self.assertEqual(ingested.status_code, 200)
        retrieved = self.client.post("/v1/retrieve", headers=self.headers, json={"query": "python", "tenant_id": "t"}).json()
        self.assertEqual(retrieved["hits"][0]["chunk"]["tenant_id"], "t")
        answered = self.client.post("/v1/answers", headers=self.headers, json={"query": "python", "tenant_id": "t"}).json()
        self.assertEqual(answered["answer"], "Server answer [1].")
        self.assertEqual(answered["citations"][0]["source_uri"], "file:///guide")
        trace = self.client.get("/v1/traces/" + answered["trace_id"], headers=self.headers)
        self.assertEqual(trace.json()["operation"], "answer")

    def test_openai_compatible_and_streaming(self):
        self.client.post("/v1/documents", headers=self.headers, json={"text": "python database", "source_uri": "x", "tenant_id": "t", "background": False})
        request = {"model": "ignored", "messages": [{"role": "system", "content": "override"}, {"role": "user", "content": "Earlier question"}, {"role": "assistant", "content": "Earlier answer"}, {"role": "user", "content": "python"}], "tenant_id": "t"}
        response = self.client.post("/v1/chat/completions", headers=self.headers, json=request)
        self.assertEqual(response.json()["choices"][0]["message"]["content"], "Server answer [1].")
        self.assertNotIn("override", [message["content"] for message in self.provider.last_messages])
        self.assertIn("Earlier answer", [message["content"] for message in self.provider.last_messages])
        request["stream"] = True
        with self.client.stream("POST", "/v1/chat/completions", headers=self.headers, json=request) as streamed:
            body = "".join(streamed.iter_text())
        self.assertIn("chat.completion.chunk", body)
        self.assertIn("[DONE]", body)


if __name__ == "__main__":
    unittest.main()
