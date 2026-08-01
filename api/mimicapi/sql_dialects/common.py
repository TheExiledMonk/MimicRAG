from __future__ import annotations

import re


def normalize_between(sql: str) -> str:
    pattern = re.compile(r"(\w+)\s+BETWEEN\s+([^\s]+)\s+AND\s+([^\s]+)", re.IGNORECASE)
    def repl(match: re.Match) -> str:
        col = match.group(1)
        lo = match.group(2)
        hi = match.group(3)
        return f"({col} >= {lo} AND {col} <= {hi})"
    return pattern.sub(repl, sql)


def normalize_in(sql: str) -> str:
    pattern = re.compile(r"(\w+)\s+IN\s*\(([^)]+)\)", re.IGNORECASE)
    def repl(match: re.Match) -> str:
        col = match.group(1)
        raw_values = match.group(2)
        if "SELECT" in raw_values.upper():
            return match.group(0)
        values = [v.strip() for v in raw_values.split(",") if v.strip()]
        if not values:
            return "FALSE"
        parts = [f"{col} = {val}" for val in values]
        return "(" + " OR ".join(parts) + ")"
    return pattern.sub(repl, sql)


def normalize_coalesce(sql: str) -> str:
    sql = re.sub(r"\bNVL\s*\(", "COALESCE(", sql, flags=re.IGNORECASE)
    sql = re.sub(r"\bIFNULL\s*\(", "COALESCE(", sql, flags=re.IGNORECASE)
    return sql


def normalize_common(sql: str) -> str:
    sql = normalize_coalesce(sql)
    sql = normalize_between(sql)
    sql = normalize_in(sql)
    sql = normalize_count_literals(sql)
    return sql


def normalize_count_literals(sql: str) -> str:
    sql = re.sub(r"COUNT\\s*\\(\\s*1\\s*\\)", "COUNT(*)", sql, flags=re.IGNORECASE)
    sql = re.sub(r"COUNT\\s*\\(\\s*0\\s*\\)", "COUNT(*)", sql, flags=re.IGNORECASE)
    return sql
