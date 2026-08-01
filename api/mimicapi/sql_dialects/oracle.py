from __future__ import annotations

import re

from .common import normalize_common


def normalize(sql: str) -> str:
    sql = normalize_common(sql)
    sql = _normalize_select_alias(sql)
    sql = _normalize_fetch_first(sql)
    sql = _normalize_rownum(sql)
    return sql


def _normalize_select_alias(sql: str) -> str:
    pattern = re.compile(
        r"SELECT\s+(\w+)\s+(\w+)\s+FROM",
        re.IGNORECASE,
    )
    return pattern.sub(r"SELECT \\1 AS \\2 FROM", sql)


def _normalize_fetch_first(sql: str) -> str:
    match = re.search(r"FETCH\s+FIRST\s+(\d+)\s+ROWS\s+ONLY", sql, re.IGNORECASE)
    if not match:
        return sql
    limit = match.group(1)
    sql = re.sub(r"FETCH\s+FIRST\s+\d+\s+ROWS\s+ONLY", f"LIMIT {limit}", sql, flags=re.IGNORECASE)
    return sql


def _normalize_rownum(sql: str) -> str:
    match = re.search(r"ROWNUM\s*<=\s*(\d+)", sql, re.IGNORECASE)
    if not match:
        return sql
    limit = match.group(1)
    sql = re.sub(r"ROWNUM\s*<=\s*\d+", "1=1", sql, flags=re.IGNORECASE)
    if " LIMIT " not in sql.upper():
        sql = f"{sql} LIMIT {limit}"
    return sql
