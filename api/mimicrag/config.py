from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
from pathlib import Path
from typing import Any, Mapping


@dataclass(frozen=True)
class ModelConfig:
    provider: str
    model: str
    api_key: str = field(default="", repr=False)
    base_url: str = ""
    api_key_env: str = ""
    timeout_seconds: float = 60.0
    max_retries: int = 2
    headers: Mapping[str, str] = field(default_factory=dict)
    api_version: str = ""

    def resolved_api_key(self, environ: Mapping[str, str] | None = None) -> str:
        env = os.environ if environ is None else environ
        if self.api_key:
            return self.api_key
        if self.api_key_env:
            return env.get(self.api_key_env, "")
        return ""

    def safe_dict(self) -> dict[str, Any]:
        return {
            "provider": self.provider,
            "model": self.model,
            "base_url": self.base_url,
            "api_key_env": self.api_key_env,
            "api_key": "***" if self.api_key else "",
            "timeout_seconds": self.timeout_seconds,
            "max_retries": self.max_retries,
            "headers": dict(self.headers),
            "api_version": self.api_version,
        }


@dataclass(frozen=True)
class RagConfig:
    chat: ModelConfig
    embedding: ModelConfig
    database: str = "mimicrag"
    tenant_id: str = "default"
    storage: "StorageConfig" = field(default_factory=lambda: StorageConfig())
    server: "ServerConfig" = field(default_factory=lambda: ServerConfig())


@dataclass(frozen=True)
class StorageConfig:
    backend: str = "memory"
    host: str = "127.0.0.1"
    port: int = 9000
    identity_key_path: str = ""


@dataclass(frozen=True)
class ServerConfig:
    host: str = "127.0.0.1"
    port: int = 8080
    api_key: str = field(default="", repr=False)
    api_key_env: str = ""
    requests_per_minute: int = 120
    max_query_chars: int = 16000
    max_document_chars: int = 10_000_000
    context_token_budget: int = 4000
    answer_max_tokens: int = 1024
    trace_path: str = ""

    def resolved_api_key(self, environ: Mapping[str, str] | None = None) -> str:
        env = os.environ if environ is None else environ
        return self.api_key or (env.get(self.api_key_env, "") if self.api_key_env else "")


_DEFAULT_URLS = {
    "openai": "https://api.openai.com/v1",
    "anthropic": "https://api.anthropic.com/v1",
    "google": "https://generativelanguage.googleapis.com/v1beta",
    "cohere": "https://api.cohere.com/v2",
    "ollama": "http://127.0.0.1:11434",
    "groq": "https://api.groq.com/openai/v1",
    "mistral": "https://api.mistral.ai/v1",
    "xai": "https://api.x.ai/v1",
    "deepseek": "https://api.deepseek.com/v1",
    "together": "https://api.together.xyz/v1",
    "azure_openai": "",
    "openai_compatible": "",
    "custom": "",
}


def _model_config(raw: Mapping[str, Any]) -> ModelConfig:
    provider = str(raw.get("provider", "openai_compatible")).lower()
    if provider not in _DEFAULT_URLS:
        raise ValueError(f"unsupported provider '{provider}'")
    model = str(raw.get("model", "")).strip()
    if not model:
        raise ValueError("model is required")
    return ModelConfig(
        provider=provider,
        model=model,
        api_key=str(raw.get("api_key", "")),
        api_key_env=str(raw.get("api_key_env", "")),
        base_url=str(raw.get("base_url", _DEFAULT_URLS[provider])).rstrip("/"),
        timeout_seconds=float(raw.get("timeout_seconds", 60.0)),
        max_retries=int(raw.get("max_retries", 2)),
        headers={str(k): str(v) for k, v in raw.get("headers", {}).items()},
        api_version=str(raw.get("api_version", "")),
    )


def load_config(path: str | Path, environ: Mapping[str, str] | None = None) -> RagConfig:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    storage_raw = raw.get("storage", {})
    server_raw = raw.get("server", {})
    config = RagConfig(
        chat=_model_config(raw["chat"]),
        embedding=_model_config(raw["embedding"]),
        database=str(raw.get("database", "mimicrag")),
        tenant_id=str(raw.get("tenant_id", "default")),
        storage=StorageConfig(
            backend=str(storage_raw.get("backend", "memory")),
            host=str(storage_raw.get("host", "127.0.0.1")),
            port=int(storage_raw.get("port", 9000)),
            identity_key_path=str(storage_raw.get("identity_key_path", "")),
        ),
        server=ServerConfig(
            host=str(server_raw.get("host", "127.0.0.1")),
            port=int(server_raw.get("port", 8080)),
            api_key=str(server_raw.get("api_key", "")),
            api_key_env=str(server_raw.get("api_key_env", "")),
            requests_per_minute=int(server_raw.get("requests_per_minute", 120)),
            max_query_chars=int(server_raw.get("max_query_chars", 16000)),
            max_document_chars=int(server_raw.get("max_document_chars", 10_000_000)),
            context_token_budget=int(server_raw.get("context_token_budget", 4000)),
            answer_max_tokens=int(server_raw.get("answer_max_tokens", 1024)),
            trace_path=str(server_raw.get("trace_path", "")),
        ),
    )
    # Fail early only when the operator explicitly selected an environment secret.
    env = os.environ if environ is None else environ
    for name, model in (("chat", config.chat), ("embedding", config.embedding)):
        if model.api_key_env and not model.resolved_api_key(env):
            raise ValueError(f"{name} API key environment variable '{model.api_key_env}' is not set")
    if config.server.api_key_env and not config.server.resolved_api_key(env):
        raise ValueError(f"server API key environment variable '{config.server.api_key_env}' is not set")
    if config.storage.backend not in {"memory", "embedded", "network"}:
        raise ValueError("storage.backend must be memory, embedded, or network")
    return config
