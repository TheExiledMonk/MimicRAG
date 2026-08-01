from __future__ import annotations

from abc import ABC, abstractmethod
import json
import time
from typing import Any, Iterator, Mapping
from urllib import error, request

from .config import ModelConfig


class ProviderError(RuntimeError):
    pass


class ModelProvider(ABC):
    capabilities: frozenset[str] = frozenset()

    @abstractmethod
    def chat(self, messages: list[dict[str, str]], **options: Any) -> str: ...

    @abstractmethod
    def embed(self, texts: list[str], **options: Any) -> list[list[float]]: ...

    def rerank(self, query: str, documents: list[str], **options: Any) -> list[tuple[int, float]]:
        raise ProviderError(f"{type(self).__name__} does not support reranking")

    def stream_chat(self, messages: list[dict[str, str]], **options: Any) -> Iterator[str]:
        yield self.chat(messages, **options)


class HttpProvider(ModelProvider):
    def __init__(self, config: ModelConfig) -> None:
        self.config = config

    def _post(self, path: str, body: Mapping[str, Any], headers: Mapping[str, str]) -> dict[str, Any]:
        if not self.config.base_url:
            raise ProviderError(f"base_url is required for provider '{self.config.provider}'")
        payload = json.dumps(body).encode("utf-8")
        merged = {"Content-Type": "application/json", **self.config.headers, **headers}
        last: Exception | None = None
        for attempt in range(self.config.max_retries + 1):
            try:
                req = request.Request(self.config.base_url + path, payload, merged, method="POST")
                with request.urlopen(req, timeout=self.config.timeout_seconds) as response:
                    return json.loads(response.read().decode("utf-8"))
            except (error.HTTPError, error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                last = exc
                if attempt < self.config.max_retries:
                    time.sleep(min(0.25 * (2 ** attempt), 2.0))
        raise ProviderError(f"{self.config.provider} request failed: {last}") from last

    def _bearer(self) -> dict[str, str]:
        key = self.config.resolved_api_key()
        return {"Authorization": f"Bearer {key}"} if key else {}

    def _stream_post(self, path: str, body: Mapping[str, Any], headers: Mapping[str, str]) -> Iterator[dict[str, Any]]:
        payload = json.dumps(body).encode("utf-8")
        merged = {"Content-Type": "application/json", **self.config.headers, **headers}
        req = request.Request(self.config.base_url + path, payload, merged, method="POST")
        try:
            with request.urlopen(req, timeout=self.config.timeout_seconds) as response:
                for raw in response:
                    line = raw.decode("utf-8").strip()
                    if not line or line.startswith(":"):
                        continue
                    if line.startswith("data:"):
                        line = line[5:].strip()
                    if line == "[DONE]":
                        return
                    try:
                        yield json.loads(line)
                    except json.JSONDecodeError:
                        continue
        except (error.HTTPError, error.URLError, TimeoutError) as exc:
            raise ProviderError(f"{self.config.provider} streaming request failed: {exc}") from exc


class OpenAICompatibleProvider(HttpProvider):
    capabilities = frozenset({"chat", "embedding"})

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        data = self._post("/chat/completions", {"model": self.config.model, "messages": messages, **options}, self._bearer())
        return str(data["choices"][0]["message"]["content"])

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        data = self._post("/embeddings", {"model": self.config.model, "input": texts, **options}, self._bearer())
        ordered = sorted(data["data"], key=lambda item: item.get("index", 0))
        return [[float(value) for value in item["embedding"]] for item in ordered]

    def stream_chat(self, messages: list[dict[str, str]], **options: Any) -> Iterator[str]:
        for event in self._stream_post("/chat/completions", {"model": self.config.model, "messages": messages, "stream": True, **options}, self._bearer()):
            choices = event.get("choices", [])
            if choices:
                text = choices[0].get("delta", {}).get("content")
                if text:
                    yield str(text)


class AzureOpenAIProvider(OpenAICompatibleProvider):
    def _path(self, value: str) -> str:
        version = self.config.api_version or "2024-10-21"
        return f"{value}?api-version={version}"

    def _azure_headers(self) -> dict[str, str]:
        key = self.config.resolved_api_key()
        return {"api-key": key} if key else {}

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        data = self._post(self._path("/chat/completions"), {"messages": messages, **options}, self._azure_headers())
        return str(data["choices"][0]["message"]["content"])

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        data = self._post(self._path("/embeddings"), {"input": texts, **options}, self._azure_headers())
        ordered = sorted(data["data"], key=lambda item: item.get("index", 0))
        return [[float(value) for value in item["embedding"]] for item in ordered]

    def stream_chat(self, messages: list[dict[str, str]], **options: Any) -> Iterator[str]:
        for event in self._stream_post(self._path("/chat/completions"), {"messages": messages, "stream": True, **options}, self._azure_headers()):
            choices = event.get("choices", [])
            if choices:
                text = choices[0].get("delta", {}).get("content")
                if text:
                    yield str(text)


class AnthropicProvider(HttpProvider):
    capabilities = frozenset({"chat"})

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        system = "\n".join(m["content"] for m in messages if m["role"] == "system")
        conversational = [m for m in messages if m["role"] != "system"]
        headers = {"x-api-key": self.config.resolved_api_key(), "anthropic-version": "2023-06-01"}
        body = {"model": self.config.model, "messages": conversational, "max_tokens": 1024, **options}
        if system:
            body["system"] = system
        data = self._post("/messages", body, headers)
        return "".join(str(block.get("text", "")) for block in data["content"] if block.get("type") == "text")

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        raise ProviderError("Anthropic does not expose an embedding API; configure a separate embedding provider")

    def stream_chat(self, messages: list[dict[str, str]], **options: Any) -> Iterator[str]:
        system = "\n".join(m["content"] for m in messages if m["role"] == "system")
        body = {"model": self.config.model, "messages": [m for m in messages if m["role"] != "system"], "max_tokens": 1024, "stream": True, **options}
        if system:
            body["system"] = system
        headers = {"x-api-key": self.config.resolved_api_key(), "anthropic-version": "2023-06-01"}
        for event in self._stream_post("/messages", body, headers):
            if event.get("type") == "content_block_delta":
                text = event.get("delta", {}).get("text")
                if text:
                    yield str(text)


class OllamaProvider(HttpProvider):
    capabilities = frozenset({"chat", "embedding"})

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        data = self._post("/api/chat", {"model": self.config.model, "messages": messages, "stream": False, **options}, self._bearer())
        return str(data["message"]["content"])

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        data = self._post("/api/embed", {"model": self.config.model, "input": texts, **options}, self._bearer())
        return [[float(value) for value in vector] for vector in data["embeddings"]]

    def stream_chat(self, messages: list[dict[str, str]], **options: Any) -> Iterator[str]:
        for event in self._stream_post("/api/chat", {"model": self.config.model, "messages": messages, "stream": True, **options}, self._bearer()):
            text = event.get("message", {}).get("content")
            if text:
                yield str(text)


class CohereProvider(HttpProvider):
    capabilities = frozenset({"chat", "embedding", "rerank"})

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        data = self._post("/chat", {"model": self.config.model, "messages": messages, **options}, self._bearer())
        return str(data["message"]["content"][0]["text"])

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        body = {"model": self.config.model, "texts": texts, "input_type": "search_document", "embedding_types": ["float"], **options}
        data = self._post("/embed", body, self._bearer())
        return [[float(value) for value in vector] for vector in data["embeddings"]["float"]]

    def rerank(self, query: str, documents: list[str], **options: Any) -> list[tuple[int, float]]:
        data = self._post("/rerank", {"model": self.config.model, "query": query, "documents": documents, "top_n": len(documents), **options}, self._bearer())
        return [(int(item["index"]), float(item["relevance_score"])) for item in data["results"]]


class GoogleProvider(HttpProvider):
    capabilities = frozenset({"chat", "embedding"})

    def _key_path(self, path: str) -> str:
        key = self.config.resolved_api_key()
        return path + (("&" if "?" in path else "?") + "key=" + key if key else "")

    def chat(self, messages: list[dict[str, str]], **options: Any) -> str:
        contents = [{"role": "model" if m["role"] == "assistant" else "user", "parts": [{"text": m["content"]}]} for m in messages]
        data = self._post(self._key_path(f"/models/{self.config.model}:generateContent"), {"contents": contents, **options}, {})
        return "".join(p.get("text", "") for p in data["candidates"][0]["content"]["parts"])

    def embed(self, texts: list[str], **options: Any) -> list[list[float]]:
        requests = [{"model": f"models/{self.config.model}", "content": {"parts": [{"text": text}]}} for text in texts]
        data = self._post(self._key_path(f"/models/{self.config.model}:batchEmbedContents"), {"requests": requests, **options}, {})
        return [[float(value) for value in item["values"]] for item in data["embeddings"]]


def create_provider(config: ModelConfig) -> ModelProvider:
    if config.provider == "azure_openai":
        return AzureOpenAIProvider(config)
    if config.provider == "anthropic":
        return AnthropicProvider(config)
    if config.provider == "ollama":
        return OllamaProvider(config)
    if config.provider == "cohere":
        return CohereProvider(config)
    if config.provider == "google":
        return GoogleProvider(config)
    # OpenAI, Azure OpenAI, and arbitrary custom URLs share this request shape.
    return OpenAICompatibleProvider(config)
