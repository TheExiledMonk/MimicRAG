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
_NET_OPS = {
    "eq": 0,
    "ne": 1,
    "lt": 2,
    "le": 3,
    "gt": 4,
    "ge": 5,
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
        self._join_spec = None

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

    def join(self, other, on: str, how: str = "inner") -> "Query":
        self._join_spec = (other, on, how)
        raise NotImplementedError("API-only joins are not implemented yet")

    def assemble(self):
        raise NotImplementedError("API-only document assembly is not implemented yet")

    def aggregate(self, debug: bool = False, **kwargs):
        if getattr(self._dataset, "_net", None) is not None:
            predicates = []
            field_name = kwargs.get("sum") or kwargs.get("min") or kwargs.get("max")
            if field_name is None:
                raise NotImplementedError("network backend requires a field for aggregation")
            if any(
                name not in (None, field_name)
                for name in (kwargs.get("sum"), kwargs.get("min"), kwargs.get("max"))
            ):
                raise NotImplementedError("network backend supports one field per aggregate")
            fields = list(self._dataset.fields.keys())
            if field_name not in fields:
                raise ValueError(f"unknown field '{field_name}'")
            for pred in self._predicates:
                if pred.field not in fields:
                    raise ValueError(f"unknown field '{pred.field}'")
                predicates.append(
                    (fields.index(pred.field), _NET_OPS[pred.op], float(pred.value))
                )
            result = self._dataset._net.query_agg(
                self._dataset.name,
                fields.index(field_name),
                database=self._dataset._database,
                predicates=predicates,
            )
            output = {}
            if kwargs.get("sum"):
                output["sum"] = result["sum"]
            if kwargs.get("min"):
                output["min"] = result["min"]
            if kwargs.get("max"):
                output["max"] = result["max"]
            if kwargs.get("count"):
                output["count"] = result["count"]
            if "rows_scanned" in result:
                output["rows_scanned"] = result["rows_scanned"]
            return output
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

    def scan(self, columns: list[str] | None = None, limit: int = 0, offset: int = 0):
        if getattr(self._dataset, "_net", None) is not None:
            predicates = []
            fields = list(self._dataset.fields.keys())
            for pred in self._predicates:
                if pred.field not in fields:
                    raise ValueError(f"unknown field '{pred.field}'")
                predicates.append(
                    (fields.index(pred.field), _NET_OPS[pred.op], float(pred.value))
                )
            return self._dataset._net.scan(
                self._dataset.name,
                fields=[(name, self._dataset.fields[name]) for name in fields],
                columns=columns,
                database=self._dataset._database,
                predicates=predicates,
                limit=limit,
                offset=offset,
            )
        if getattr(self._dataset, "_cpp", None) is not None:
            predicates = [(p.field, p.op, p.value) for p in self._predicates]
            return self._dataset._cpp.scan(predicates, columns, limit, offset)
        return self._scan_python(columns=columns, limit=limit, offset=offset)

    def execute(self, debug: bool = False):
        if getattr(self._dataset, "_net", None) is not None:
            return self.aggregate(debug=debug)
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

    def _scan_python(self, columns: list[str] | None, limit: int, offset: int):
        if columns is None:
            columns = list(self._dataset.fields.keys())
        mask = self._apply_predicates()
        count = self._dataset._row_count()
        rows = []
        skipped = 0
        added = 0
        for i in range(count):
            if not mask[i]:
                continue
            if skipped < offset:
                skipped += 1
                continue
            row = {}
            for name in columns:
                valid = self._dataset._valid(name)
                if not valid[i]:
                    row[name] = None
                else:
                    row[name] = self._dataset._column(name)[i]
            rows.append(row)
            added += 1
            if limit and added >= limit:
                break
        return rows

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
