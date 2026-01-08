from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal


FilterOp = Literal["EQ", "NEQ", "LT", "LTE", "GT", "GTE", "IS_NULL", "IS_NOT_NULL"]
SortDirection = Literal["ASC", "DESC"]
BooleanOp = Literal["LEAF", "AND", "OR", "NOT"]
GroupingMode = Literal["first", "error"]


@dataclass
class Projection:
    column: str
    alias: str | None = None
    expression: str | None = None


@dataclass
class Filter:
    column: str
    op: FilterOp
    value: object | None = None


@dataclass
class FilterExpr:
    op: BooleanOp = "LEAF"
    filter: Filter | None = None
    children: list["FilterExpr"] = field(default_factory=list)


@dataclass
class OrderKey:
    column: str
    direction: SortDirection = "ASC"


@dataclass
class CanonicalQuery:
    projections: list[Projection]
    dataset: str
    filters: FilterExpr | None
    group_keys: list[str]
    having_filters: FilterExpr | None
    order_keys: list[OrderKey]
    limit: int | None = None
    offset: int | None = None
    grouping_mode: GroupingMode = "first"
    aggregates: list[Aggregate] = field(default_factory=list)


_FILTER_OPS: set[str] = {
    "EQ",
    "NEQ",
    "LT",
    "LTE",
    "GT",
    "GTE",
    "IS_NULL",
    "IS_NOT_NULL",
}

_SORT_DIRS: set[str] = {"ASC", "DESC"}


def validate_query(query: CanonicalQuery) -> None:
    if not query.dataset:
        raise ValueError("dataset is required")
    if query.limit is not None and query.limit < 0:
        raise ValueError("limit must be non-negative")
    if query.offset is not None and query.offset < 0:
        raise ValueError("offset must be non-negative")
    if query.grouping_mode not in ("first", "error"):
        raise ValueError("grouping_mode must be 'first' or 'error'")
    for root in (query.filters, query.having_filters):
        if root:
            _validate_filter_expr(root)
    for order in query.order_keys:
        if order.direction not in _SORT_DIRS:
            raise ValueError(f"unsupported order direction '{order.direction}'")


def _validate_filter_expr(expr: FilterExpr) -> None:
    if expr.op == "LEAF":
        if not expr.filter:
            raise ValueError("filter leaf missing filter")
        if expr.filter.op not in _FILTER_OPS:
            raise ValueError(f"unsupported filter op '{expr.filter.op}'")
        return
    if not expr.children:
        raise ValueError("boolean filter missing children")
    for child in expr.children:
        _validate_filter_expr(child)


def serialize_query(query: CanonicalQuery) -> dict:
    validate_query(query)
    return {
        "projections": [
            {
                "column": proj.column,
                "alias": proj.alias,
                "expression": proj.expression,
            }
            for proj in query.projections
        ],
        "dataset": query.dataset,
        "filters": _serialize_filter_expr(query.filters) if query.filters else None,
        "group_keys": list(query.group_keys),
        "having_filters": _serialize_filter_expr(query.having_filters)
        if query.having_filters else None,
        "order_keys": [
            {"column": key.column, "direction": key.direction}
            for key in query.order_keys
        ],
        "limit": query.limit,
        "offset": query.offset,
        "grouping_mode": query.grouping_mode,
        "aggregates": [
            {"kind": agg.kind, "column": agg.column, "alias": agg.alias}
            for agg in query.aggregates
        ],
    }


def _serialize_filter_expr(expr: FilterExpr) -> dict:
    if expr.op == "LEAF":
        if not expr.filter:
            return {"op": "LEAF", "filter": None}
        return {
            "op": "LEAF",
            "filter": {
                "column": expr.filter.column,
                "op": expr.filter.op,
                "value": expr.filter.value,
            },
        }
    return {
        "op": expr.op,
        "children": [_serialize_filter_expr(child) for child in expr.children],
    }
AggregateKind = Literal["COUNT", "COUNT_NONNULL", "SUM", "MIN", "MAX", "AVG"]
@dataclass
class Aggregate:
    kind: AggregateKind
    column: str | None = None
    alias: str | None = None
