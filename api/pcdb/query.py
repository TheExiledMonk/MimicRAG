from __future__ import annotations

from dataclasses import dataclass
from typing import Callable


_OPS: dict[str, Callable[[float, float], bool]] = {
    "eq": lambda left, right: left == right,
    "ne": lambda left, right: left != right,
    "lt": lambda left, right: left < right,
    "le": lambda left, right: left <= right,
    "gt": lambda left, right: left > right,
    "ge": lambda left, right: left >= right,
}


@dataclass(frozen=True)
class Predicate:
    field: str
    op: str
    value: object


class Query:
    def __init__(self, dataset):
        self._dataset = dataset
        self._predicates: list[Predicate] = []

    def _with_predicates(self, kwargs) -> "Query":
        for key, value in kwargs.items():
            if "_" not in key:
                raise ValueError("predicate must include an operator suffix")
            field, op = key.rsplit("_", 1)
            if op not in _OPS:
                raise ValueError(f"unsupported operator: {op}")
            self._predicates.append(Predicate(field, op, value))
        return self

    def filter(self, **kwargs) -> "Query":
        return self._with_predicates(kwargs)

    def aggregate(self, debug: bool = False, **kwargs):
        if getattr(self._dataset, "_cpp", None) is not None:
            predicates = [(p.field, p.op, p.value) for p in self._predicates]
            if debug:
                return self._dataset._cpp.aggregate_debug(predicates, kwargs)
            return self._dataset._cpp.aggregate(predicates, kwargs)
        result = self._aggregate(**kwargs)
        if debug:
            stats = {
                "segments_total": 1,
                "segments_scanned": 1,
                "segments_pruned": 0,
            }
            return result, stats
        return result

    def execute(self, debug: bool = False):
        if getattr(self._dataset, "_cpp", None) is not None:
            predicates = [(p.field, p.op, p.value) for p in self._predicates]
            if debug:
                return self._dataset._cpp.aggregate_debug(predicates, {})
            return self._dataset._cpp.aggregate(predicates, {})
        result = self._aggregate()
        if debug:
            stats = {
                "segments_total": 1,
                "segments_scanned": 1,
                "segments_pruned": 0,
            }
            return result, stats
        return result

    def _apply_predicates(self) -> list[bool]:
        count = self._dataset._row_count()
        mask = [True] * count
        for pred in self._predicates:
            column = self._dataset._column(pred.field)
            valid = self._dataset._valid(pred.field)
            op_fn = _OPS[pred.op]
            for i in range(count):
                if not mask[i]:
                    continue
                if not valid[i]:
                    mask[i] = False
                    continue
                mask[i] = op_fn(column[i], pred.value)
        return mask

    def _aggregate(self, **kwargs):
        if getattr(self._dataset, "_cpp", None) is not None:
            predicates = [(p.field, p.op, p.value) for p in self._predicates]
            return self._dataset._cpp.aggregate(predicates, kwargs)
        mask = self._apply_predicates()
        count = self._dataset._row_count()
        result = {}

        sum_field = kwargs.get("sum")
        if sum_field:
            total = 0.0
            valid = self._dataset._valid(sum_field)
            column = self._dataset._column(sum_field)
            for i in range(count):
                if mask[i] and valid[i]:
                    total += float(column[i])
            result["sum"] = total

        min_field = kwargs.get("min")
        if min_field:
            min_val = None
            valid = self._dataset._valid(min_field)
            column = self._dataset._column(min_field)
            for i in range(count):
                if mask[i] and valid[i]:
                    value = column[i]
                    if min_val is None or value < min_val:
                        min_val = value
            result["min"] = min_val

        max_field = kwargs.get("max")
        if max_field:
            max_val = None
            valid = self._dataset._valid(max_field)
            column = self._dataset._column(max_field)
            for i in range(count):
                if mask[i] and valid[i]:
                    value = column[i]
                    if max_val is None or value > max_val:
                        max_val = value
            result["max"] = max_val

        if kwargs.get("count"):
            if min_field:
                valid = self._dataset._valid(min_field)
                result["count"] = sum(1 for i in range(count) if mask[i] and valid[i])
            elif max_field:
                valid = self._dataset._valid(max_field)
                result["count"] = sum(1 for i in range(count) if mask[i] and valid[i])
            else:
                result["count"] = sum(1 for i in range(count) if mask[i])

        return result
