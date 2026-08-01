from __future__ import annotations

from dataclasses import dataclass
import os


@dataclass(frozen=True)
class ServiceConfig:
    bind_host: str = os.getenv("MIMICDB_UI_BIND_HOST", "127.0.0.1")
    bind_port: int = int(os.getenv("MIMICDB_UI_BIND_PORT", "8000"))
    default_database: str = os.getenv("MIMICDB_UI_DEFAULT_DB", "default")
    storage_root: str = os.getenv("MIMICDB_UI_STORAGE_ROOT", "./data")
    max_scan_limit: int = int(os.getenv("MIMICDB_UI_MAX_SCAN_LIMIT", "10000"))
    max_scan_cells: int = int(os.getenv("MIMICDB_UI_MAX_SCAN_CELLS", "200000"))
    identity_key_path: str = os.getenv("MIMICDB_UI_IDENTITY_KEY", "")
