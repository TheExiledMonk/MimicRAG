from __future__ import annotations

from dataclasses import dataclass

from .api_client import ApiClient


@dataclass
class SessionConfig:
    blacklist_ttl_sec: float = 5.0
    max_retries: int = 2
    debug: bool = False


class Session:
    def __init__(self, client: ApiClient, config: SessionConfig | None = None) -> None:
        self._client = client
        self._config = config or SessionConfig()
        self._cancelled = False
        self._apply_config()

    def _apply_config(self) -> None:
        self._client.set_blacklist_ttl(self._config.blacklist_ttl_sec)
        self._client.set_max_retries(self._config.max_retries)
        self._client.set_debug(self._config.debug)

    def cancel(self) -> None:
        self._cancelled = True

    def cancelled(self) -> bool:
        return self._cancelled

    def client(self) -> ApiClient:
        return self._client

    def config(self) -> SessionConfig:
        return self._config
