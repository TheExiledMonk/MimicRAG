from __future__ import annotations

import re

from .sql_ast import Aggregate, CanonicalQuery, Filter, FilterExpr, OrderKey, Projection
from .sql_dialects.common import normalize_common
from .sql_dialects.postgres import normalize as normalize_postgres
from .sql_dialects.mysql import normalize as normalize_mysql
from .sql_dialects.sqlite import normalize as normalize_sqlite
from .sql_dialects.oracle import normalize as normalize_oracle
from .sql_dialects.duckdb import normalize as normalize_duckdb
from .sql_dialects.sqlserver import normalize as normalize_sqlserver


_SELECT_RE = re.compile(
    r"^\s*SELECT\s+(?P<select>.+?)\s+FROM\s+(?P<from>\S+)(?P<tail>.*)$",
    re.IGNORECASE | re.DOTALL,
)


_DIALECT_NORMALIZERS = {
    "ansi": normalize_common,
    "postgres": normalize_postgres,
    "postgresql": normalize_postgres,
    "mysql": normalize_mysql,
    "mariadb": normalize_mysql,
    "sqlite": normalize_sqlite,
    "oracle": normalize_oracle,
    "duckdb": normalize_duckdb,
    "sqlserver": normalize_sqlserver,
    "tsql": normalize_sqlserver,
}


def parse_sql(query: str, dialect: str = "ansi") -> CanonicalQuery:
    normalizer = _DIALECT_NORMALIZERS.get(dialect.lower())
    if not normalizer:
        raise ValueError(f"unknown SQL dialect '{dialect}'")
    normalized = normalizer(query)
    _reject_unsupported(normalized)
    match = _SELECT_RE.match(normalized.strip())
    if not match:
        raise ValueError("invalid SQL query")
    select_part = match.group("select").strip()
    from_part = _strip_identifier(match.group("from").strip())
    tail = match.group("tail") or ""
    clauses = _split_clauses(tail)
    projections, aggregates = _parse_select_list(select_part)
    filters = _parse_where_clause(clauses.get("where"))
    group_keys = _resolve_group_by(
        _parse_group_by(clauses.get("group by")),
        projections,
    )
    having_filters = _parse_having_clause(clauses.get("having"))
    order_keys = _resolve_order_by(
        _parse_order_by(clauses.get("order by")),
        projections,
    )
    limit = _parse_limit(clauses.get("limit"))
    offset = _parse_offset(clauses.get("offset"))
    return CanonicalQuery(
        projections=projections,
        dataset=from_part,
        filters=filters,
        group_keys=group_keys,
        having_filters=having_filters,
        order_keys=order_keys,
        limit=limit,
        offset=offset,
        aggregates=aggregates,
    )


def _split_clauses(tail: str) -> dict[str, str]:
    keys = ["where", "group by", "having", "order by", "limit", "offset"]
    upper = tail.upper()
    positions: list[tuple[int, str]] = []
    for key in keys:
        idx = upper.find(f" {key.upper()} ")
        if idx >= 0:
            positions.append((idx, key))
    positions.sort()
    if not positions:
        return {}
    out: dict[str, str] = {}
    for i, (start, key) in enumerate(positions):
        end = positions[i + 1][0] if i + 1 < len(positions) else len(tail)
        segment = tail[start:end].strip()
        out[key] = segment[len(key):].strip()
    return out


def _parse_select_list(text: str) -> tuple[list[Projection], list[Aggregate]]:
    parts = _split_csv(text)
    projections: list[Projection] = []
    aggregates: list[Aggregate] = []
    for part in parts:
        alias = None
        expr = part.strip()
        tokens = expr.split()
        if len(tokens) >= 3 and tokens[-2].upper() == "AS":
            alias = _strip_identifier(tokens[-1])
            expr = " ".join(tokens[:-2]).strip()
        elif len(tokens) == 2:
            alias = _strip_identifier(tokens[-1])
            expr = tokens[0]
        expr = _strip_identifier(expr)
        aggregate = _parse_aggregate(expr, alias)
        if aggregate:
            aggregates.append(aggregate)
            projections.append(Projection(column=alias or expr, alias=alias, expression=expr))
            continue
        expression = None
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", expr):
            expression = expr
        projections.append(Projection(column=expr, alias=alias, expression=expression))
    return projections, aggregates


def _parse_aggregate(expr: str, alias: str | None) -> Aggregate | None:
    match = re.match(r"(?i)^(COUNT|SUM|MIN|MAX|AVG)\\((.*?)\\)$", expr)
    if not match:
        return None
    func = match.group(1).upper()
    arg = match.group(2).strip()
    if func == "COUNT":
        if arg == "*" or arg in ("1", "0"):
            return Aggregate(kind="COUNT", column=None, alias=alias)
        return Aggregate(kind="COUNT_NONNULL", column=_strip_identifier(arg), alias=alias)
    return Aggregate(kind=func, column=_strip_identifier(arg), alias=alias)


def _parse_where_clause(text: str | None) -> FilterExpr | None:
    if not text:
        return None
    return _parse_boolean_expr(text)


def _parse_condition(text: str) -> Filter:
    upper = text.upper()
    if upper.endswith("IS NULL"):
        column = _strip_identifier(text[:-7].strip())
        return Filter(column=column, op="IS_NULL")
    if upper.endswith("IS NOT NULL"):
        column = _strip_identifier(text[:-11].strip())
        return Filter(column=column, op="IS_NOT_NULL")
    for op, name in ((">=", "GTE"), ("<=", "LTE"), ("<>", "NEQ"), ("!=", "NEQ"),
                     (">", "GT"), ("<", "LT"), ("=", "EQ")):
        if op in text:
            left, right = text.split(op, 1)
            return Filter(
                column=_strip_identifier(left.strip()),
                op=name,
                value=_parse_value(right.strip()),
            )
    raise ValueError(f"unsupported WHERE predicate '{text}'")


def _parse_group_by(text: str | None) -> list[str]:
    if not text:
        return []
    return [_strip_identifier(item.strip()) for item in _split_csv(text)]


def _parse_having_clause(text: str | None) -> FilterExpr | None:
    if not text:
        return None
    return _parse_boolean_expr(text)


def _parse_boolean_expr(text: str) -> FilterExpr:
    or_parts = re.split(r"\s+OR\s+", text, flags=re.IGNORECASE)
    if len(or_parts) > 1:
        return FilterExpr(
            op="OR",
            children=[_parse_boolean_expr(part.strip()) for part in or_parts if part.strip()],
        )
    and_parts = re.split(r"\s+AND\s+", text, flags=re.IGNORECASE)
    if len(and_parts) > 1:
        return FilterExpr(
            op="AND",
            children=[_parse_boolean_term(part.strip()) for part in and_parts if part.strip()],
        )
    return _parse_boolean_term(text.strip())


def _parse_boolean_term(text: str) -> FilterExpr:
    if not text:
        return FilterExpr(op="LEAF", filter=_parse_condition("1=1"))
    if text.upper().startswith("NOT "):
        inner = text[4:].strip()
        return FilterExpr(op="NOT", children=[_parse_boolean_expr(inner)])
    return FilterExpr(op="LEAF", filter=_parse_condition(text))


def _parse_order_by(text: str | None) -> list[OrderKey]:
    if not text:
        return []
    keys: list[OrderKey] = []
    for part in _split_csv(text):
        tokens = part.strip().split()
        if not tokens:
            continue
        column = tokens[0]
        direction = "ASC"
        if len(tokens) > 1 and tokens[1].upper() == "DESC":
            direction = "DESC"
        keys.append(OrderKey(column=column, direction=direction))
    return keys


def _parse_limit(text: str | None) -> int | None:
    if not text:
        return None
    return int(text.strip())


def _parse_offset(text: str | None) -> int | None:
    if not text:
        return None
    return int(text.strip())


def _parse_value(raw: str):
    raw = raw.strip()
    if raw.upper() == "NULL":
        return None
    if raw.startswith("'") and raw.endswith("'"):
        return raw[1:-1].replace("''", "'")
    try:
        if "." in raw:
            return float(raw)
        return int(raw)
    except ValueError:
        return _strip_identifier(raw)


def _split_csv(text: str) -> list[str]:
    parts: list[str] = []
    current = []
    depth = 0
    in_string = False
    iterator = iter(text)
    for ch in iterator:
        if ch == "'" and not in_string:
            in_string = True
            current.append(ch)
            continue
        if ch == "'" and in_string:
            in_string = False
            current.append(ch)
            continue
        if ch == "," and depth == 0 and not in_string:
            part = "".join(current).strip()
            if part:
                parts.append(part)
            current = []
            continue
        if ch == "(" and not in_string:
            depth += 1
        elif ch == ")" and not in_string and depth > 0:
            depth -= 1
        current.append(ch)
    tail = "".join(current).strip()
    if tail:
        parts.append(tail)
    return parts


def _strip_identifier(value: str) -> str:
    if not value:
        return value
    if value[0] in ("`", '"', "[") and value[-1] in ("`", '"', "]"):
        return value[1:-1]
    return value


def _reject_unsupported(sql: str) -> None:
    upper = sql.upper()
    if " JOIN " in upper:
        raise ValueError("JOINs are not supported")
    if " WINDOW " in upper or " OVER " in upper:
        raise ValueError("window functions are not supported")
    if " PROCEDURE " in upper or " FUNCTION " in upper or " TRIGGER " in upper:
        raise ValueError("stored procedures/triggers are not supported")
    if " INDEX " in upper or " HINT " in upper:
        raise ValueError("index/optimizer hints are not supported")
    if " TRANSACTION " in upper or " COMMIT " in upper or " ROLLBACK " in upper:
        raise ValueError("transactions are not supported")
    if " LOCK " in upper or " FOR UPDATE" in upper:
        raise ValueError("locking clauses are not supported")
    if re.search(r"\(\s*SELECT", upper):
        raise ValueError("subqueries are not supported")


def _resolve_order_by(
    order_keys: list[OrderKey],
    projections: list[Projection],
) -> list[OrderKey]:
    resolved: list[OrderKey] = []
    aliases = {proj.alias: proj.column for proj in projections if proj.alias}
    for key in order_keys:
        name = _strip_identifier(key.column)
        if name.isdigit():
            position = int(name) - 1
            if position < 0 or position >= len(projections):
                raise ValueError("ORDER BY position out of range")
            name = projections[position].alias or projections[position].column
        if name in aliases:
            name = aliases[name]
        resolved.append(OrderKey(column=name, direction=key.direction))
    return resolved


def _resolve_group_by(
    group_keys: list[str],
    projections: list[Projection],
) -> list[str]:
    aliases = {proj.alias: proj.column for proj in projections if proj.alias}
    resolved: list[str] = []
    for key in group_keys:
        name = _strip_identifier(key)
        if name in aliases:
            name = aliases[name]
        resolved.append(name)
    return resolved


__all__ = ["parse_sql"]
