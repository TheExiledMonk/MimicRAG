from __future__ import annotations

import re

from .common import normalize_common


def normalize(sql: str) -> str:
    sql = normalize_common(sql)
    return _normalize_top(sql)


def _normalize_top(sql: str) -> str:
    match = re.search(r"SELECT\\s+TOP\\s+(\\d+)", sql, re.IGNORECASE)
    if not match:
        return sql
    limit = match.group(1)
    sql = re.sub(r"SELECT\\s+TOP\\s+\\d+", "SELECT", sql, flags=re.IGNORECASE)
    if " LIMIT " not in sql.upper():
        sql = f"{sql} LIMIT {limit}"
    return sql
