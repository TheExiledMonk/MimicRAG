from __future__ import annotations

from .api_client import ApiClient
from .sql_ast import CanonicalQuery, Filter, FilterExpr, validate_query


_OP_TO_CODE = {
    "EQ": 0,
    "NEQ": 1,
    "LT": 2,
    "LTE": 3,
    "GT": 4,
    "GTE": 5,
}


def _compile_predicates(
    fields: list[tuple[str, str]],
    filters: list[Filter],
) -> list[tuple[int, int, float]]:
    field_names = [name for name, _ in fields]
    field_types = {name: ftype.upper() for name, ftype in fields}
    predicates: list[tuple[int, int, float]] = []
    for filt in filters:
        if filt.op in ("IS_NULL", "IS_NOT_NULL"):
            raise NotImplementedError("NULL filters not supported in scan predicates yet")
        if filt.column not in field_names:
            raise ValueError(f"unknown column '{filt.column}'")
        if filt.op not in _OP_TO_CODE:
            raise ValueError(f"unsupported filter op '{filt.op}'")
        if filt.value is None:
            raise ValueError("filter value is required")
        column_type = field_types.get(filt.column, "")
        if column_type not in ("INT32", "INT64", "FLOAT64", "BOOL"):
            raise ValueError("non-numeric filter on non-numeric column")
        try:
            value = float(filt.value)
        except (TypeError, ValueError) as exc:
            raise ValueError("non-numeric filter value") from exc
        predicates.append((field_names.index(filt.column), _OP_TO_CODE[filt.op], value))
    return predicates


def _flatten_filters(expr: FilterExpr | None) -> list[Filter]:
    if expr is None:
        return []
    if expr.op == "LEAF":
        return [expr.filter] if expr.filter else []
    if expr.op == "AND":
        flattened: list[Filter] = []
        for child in expr.children:
            flattened.extend(_flatten_filters(child))
        return flattened
    raise NotImplementedError("OR/NOT filter execution not supported in canonical executor")


def _eval_filter_expr(expr: FilterExpr | None, row: dict) -> bool:
    if expr is None:
        return True
    if expr.op == "LEAF":
        if not expr.filter:
            return True
        return _eval_filter(expr.filter, row)
    if expr.op == "AND":
        return all(_eval_filter_expr(child, row) for child in expr.children)
    if expr.op == "OR":
        return any(_eval_filter_expr(child, row) for child in expr.children)
    if expr.op == "NOT":
        if not expr.children:
            return True
        return not _eval_filter_expr(expr.children[0], row)
    return True


def _eval_filter(filt: Filter, row: dict) -> bool:
    value = row.get(filt.column)
    if filt.op == "IS_NULL":
        return value is None
    if filt.op == "IS_NOT_NULL":
        return value is not None
    if value is None:
        return False
    if filt.op == "EQ":
        return value == filt.value
    if filt.op == "NEQ":
        return value != filt.value
    try:
        left = float(value)
        right = float(filt.value) if filt.value is not None else None
    except (TypeError, ValueError):
        return False
    if right is None:
        return False
    if filt.op == "LT":
        return left < right
    if filt.op == "LTE":
        return left <= right
    if filt.op == "GT":
        return left > right
    if filt.op == "GTE":
        return left >= right
    return False


def execute_query(
    client: ApiClient,
    db: str,
    query: CanonicalQuery,
    fields: list[tuple[str, str]],
) -> tuple[list[dict], dict]:
    validate_query(query)
    if query.group_keys:
        raise NotImplementedError("GROUP BY not wired in canonical executor")
    if query.order_keys:
        raise NotImplementedError("ORDER BY not wired in canonical executor")
    field_names = [name for name, _ in fields]
    if query.aggregates:
        return _execute_aggregates(client, db, query, fields)
    if query.projections:
        columns = []
        for proj in query.projections:
            if proj.expression is not None:
                continue
            if proj.column not in field_names:
                raise ValueError(f"unknown column '{proj.column}'")
            columns.append(proj.column)
    else:
        columns = None
    flat_filters = _flatten_filters(query.filters)
    predicates = _compile_predicates(fields, flat_filters) if flat_filters else None
    rows, stats = client.scan_routed(
        db,
        query.dataset,
        fields,
        columns=columns,
        predicates=predicates,
        limit=query.limit or 0,
        offset=query.offset or 0,
    )
    if query.having_filters:
        rows = [row for row in rows if _eval_filter_expr(query.having_filters, row)]
    return rows, stats


def _execute_aggregates(
    client: ApiClient,
    db: str,
    query: CanonicalQuery,
    fields: list[tuple[str, str]],
) -> tuple[list[dict], dict]:
    if query.group_keys:
        raise NotImplementedError("GROUP BY aggregates not wired in canonical executor")
    field_names = [name for name, _ in fields]
    flat_filters = _flatten_filters(query.filters)
    predicates = _compile_predicates(fields, flat_filters) if flat_filters else None
    out: dict[str, float] = {}
    stats: dict[str, float] = {}
    for agg in query.aggregates:
        if agg.kind in ("COUNT",):
            result, stats = client.query_agg_routed(
                db,
                query.dataset,
                field_index=0,
                predicates=predicates,
            )
            out[agg.alias or "count"] = result.get("count", 0.0)
        elif agg.kind in ("COUNT_NONNULL", "SUM", "MIN", "MAX", "AVG"):
            if agg.column is None or agg.column not in field_names:
                raise ValueError("aggregate column must be present in schema")
            field_index = field_names.index(agg.column)
            result, stats = client.query_agg_routed(
                db,
                query.dataset,
                field_index=field_index,
                predicates=predicates,
            )
            if agg.kind == "COUNT_NONNULL":
                out[agg.alias or f"count_{agg.column}"] = result.get("count", 0.0)
            elif agg.kind == "SUM":
                out[agg.alias or f"sum_{agg.column}"] = result.get("sum", 0.0)
            elif agg.kind == "MIN":
                out[agg.alias or f"min_{agg.column}"] = result.get("min", 0.0)
            elif agg.kind == "MAX":
                out[agg.alias or f"max_{agg.column}"] = result.get("max", 0.0)
            elif agg.kind == "AVG":
                count = result.get("count", 0.0)
                total = result.get("sum", 0.0)
                out[agg.alias or f"avg_{agg.column}"] = (total / count) if count else 0.0
        else:
            raise NotImplementedError("unsupported aggregate")
    return [out], stats
