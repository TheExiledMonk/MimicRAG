from __future__ import annotations

from .common import normalize_common


def normalize(sql: str) -> str:
    return normalize_common(sql)
