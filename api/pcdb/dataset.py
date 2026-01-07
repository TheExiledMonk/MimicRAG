from __future__ import annotations

try:
    from . import _pcdb
except ImportError:
    _pcdb = None

from .query import Query


_FIELD_TYPES: dict[str, type] = {
    "int32": int,
    "int64": int,
    "float64": float,
    "bool": bool,
}
_LOCK_MODES = {"append-only", "update-only", "full-crud"}


class Dataset:
    def __init__(
        self,
        name: str,
        fields: dict[str, str],
        use_cpp: bool = True,
        lock_mode: str = "append-only",
    ) -> None:
        if lock_mode not in _LOCK_MODES:
            raise ValueError(
                f"unsupported lock_mode '{lock_mode}', expected one of: "
                f"{', '.join(sorted(_LOCK_MODES))}"
            )
        self._name = name
        self._fields = fields
        self._lock_mode = lock_mode
        self._cpp = None
        self._columns: dict[str, list] = {}
        self._validity: dict[str, list[bool]] = {}
        if use_cpp and _pcdb is not None:
            self._cpp = _pcdb.Dataset(name=name, fields=fields)
        else:
            self._columns = {key: [] for key in fields}
            self._validity = {key: [] for key in fields}

    @property
    def name(self) -> str:
        return self._name

    @property
    def fields(self) -> dict[str, str]:
        return dict(self._fields)

    @property
    def lock_mode(self) -> str:
        return self._lock_mode

    def append(self, **kwargs) -> None:
        if self._lock_mode not in ("append-only", "full-crud"):
            raise RuntimeError(f"append not allowed for lock_mode '{self._lock_mode}'")
        if self._cpp is not None:
            self._cpp.append(**kwargs)
            return
        if set(kwargs.keys()) != set(self._fields.keys()):
            raise ValueError("append requires values for all fields")
        for key, type_name in self._fields.items():
            value = kwargs[key]
            if value is None:
                self._columns[key].append(None)
                self._validity[key].append(False)
                continue
            expected = _FIELD_TYPES.get(type_name)
            if expected is None:
                raise ValueError(f"unsupported type: {type_name}")
            if expected is bool and not isinstance(value, bool):
                raise TypeError(f"field '{key}' expects bool")
            if expected in (int, float) and not isinstance(value, (int, float)):
                raise TypeError(f"field '{key}' expects {type_name}")
            if expected is int and isinstance(value, bool):
                raise TypeError(f"field '{key}' expects {type_name}")
            if expected is float and isinstance(value, bool):
                raise TypeError(f"field '{key}' expects {type_name}")
            if expected is int:
                value = int(value)
            if expected is float:
                value = float(value)
            self._columns[key].append(value)
            self._validity[key].append(True)

    def append_batch(self, columns: dict[str, list]) -> None:
        if self._lock_mode not in ("append-only", "full-crud"):
            raise RuntimeError(f"append_batch not allowed for lock_mode '{self._lock_mode}'")
        if self._cpp is not None:
            self._cpp.append_batch(columns)
            return
        if set(columns.keys()) != set(self._fields.keys()):
            raise ValueError("append_batch requires values for all fields")
        lengths = {len(values) for values in columns.values()}
        if len(lengths) != 1:
            raise ValueError("all columns must have the same length")
        count = next(iter(lengths))
        for key, type_name in self._fields.items():
            values = columns[key]
            expected = _FIELD_TYPES.get(type_name)
            if expected is None:
                raise ValueError(f"unsupported type: {type_name}")
            col_out = self._columns[key]
            valid_out = self._validity[key]
            for value in values:
                if value is None:
                    col_out.append(None)
                    valid_out.append(False)
                    continue
                if expected is bool and not isinstance(value, bool):
                    raise TypeError(f"field '{key}' expects bool")
                if expected in (int, float) and not isinstance(value, (int, float)):
                    raise TypeError(f"field '{key}' expects {type_name}")
                if expected is int and isinstance(value, bool):
                    raise TypeError(f"field '{key}' expects {type_name}")
                if expected is float and isinstance(value, bool):
                    raise TypeError(f"field '{key}' expects {type_name}")
                if expected is int:
                    value = int(value)
                if expected is float:
                    value = float(value)
                col_out.append(value)
                valid_out.append(True)
        if count == 0:
            return

    def filter(self, **kwargs) -> Query:
        return Query(self)._with_predicates(kwargs)

    def aggregate(self, **kwargs):
        return Query(self).aggregate(**kwargs)

    def query(self, debug: bool = False, **kwargs):
        query = Query(self)
        if kwargs:
            query = query._with_predicates(kwargs)
        return query.execute(debug=debug)

    def _row_count(self) -> int:
        if self._cpp is not None:
            raise RuntimeError("row_count unavailable for C++ backend")
        if not self._columns:
            return 0
        first = next(iter(self._columns.values()))
        return len(first)

    def _column(self, name: str) -> list:
        if self._cpp is not None:
            raise RuntimeError("column access unavailable for C++ backend")
        return self._columns[name]

    def _valid(self, name: str) -> list[bool]:
        if self._cpp is not None:
            raise RuntimeError("validity access unavailable for C++ backend")
        return self._validity[name]
