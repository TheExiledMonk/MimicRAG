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
        }


@dataclass(frozen=True)
class RagConfig:
    chat: ModelConfig
    embedding: ModelConfig
    database: str = "mimicrag"
    tenant_id: str = "default"


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
    )


def load_config(path: str | Path, environ: Mapping[str, str] | None = None) -> RagConfig:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    config = RagConfig(
        chat=_model_config(raw["chat"]),
        embedding=_model_config(raw["embedding"]),
        database=str(raw.get("database", "mimicrag")),
        tenant_id=str(raw.get("tenant_id", "default")),
    )
    # Fail early only when the operator explicitly selected an environment secret.
    env = os.environ if environ is None else environ
    for name, model in (("chat", config.chat), ("embedding", config.embedding)):
        if model.api_key_env and not model.resolved_api_key(env):
            raise ValueError(f"{name} API key environment variable '{model.api_key_env}' is not set")
    return config
