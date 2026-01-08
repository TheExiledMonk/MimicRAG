from __future__ import annotations

from .sql_ast import CanonicalQuery
from .sql_exec import execute_query


def execute(query: CanonicalQuery, client, db: str, fields):
    return execute_query(client, db, query, fields)


__all__ = ["execute", "CanonicalQuery"]
