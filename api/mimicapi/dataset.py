from __future__ import annotations

from pathlib import Path
import sys

try:
    from . import _mimicdb
except ImportError:
    _mimicdb = None

from .query import Query


_FIELD_TYPES: dict[str, type] = {
    "int32": int,
    "int64": int,
    "float64": float,
    "bool": bool,
    "string": str,
    "bytes": bytes,
    "vector_float32": list,
}
_LOCK_MODES = {"append-only", "update-only", "full-crud"}


def _load_network_client():
    try:
        from client.mimicdb_client import MimicDBClient, ProtocolError  # type: ignore
        return MimicDBClient, ProtocolError
    except Exception:
        repo_root = Path(__file__).resolve().parents[2]
        if str(repo_root) not in sys.path:
            sys.path.insert(0, str(repo_root))
        from client.mimicdb_client import MimicDBClient, ProtocolError  # type: ignore
        return MimicDBClient, ProtocolError


class Dataset:
    def __init__(
        self,
        name: str,
        fields: dict[str, str],
        use_cpp: bool = True,
        lock_mode: str = "append-only",
        host: str | None = None,
        port: int | None = None,
        database: str = "default",
        create: bool = True,
        identity_key_path: str | None = None,
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
        self._net = None
        self._database = database
        self._columns: dict[str, list] = {}
        self._validity: dict[str, list[bool]] = {}
        if host is not None or port is not None:
            MimicDBClient, _ = _load_network_client()
            self._net = MimicDBClient(
                host=host or "127.0.0.1",
                port=port or 9000,
                default_db=database,
                identity_key_path=identity_key_path,
            )
            if create:
                self._net.create_database(database)
                fields_list = [(key, value) for key, value in fields.items()]
                self._net.create_dataset(name, fields_list, database=database)
        elif use_cpp and _mimicdb is not None:
            self._cpp = _mimicdb.Dataset(name=name, fields=fields)
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
        if self._net is not None:
            if set(kwargs.keys()) != set(self._fields.keys()):
                raise ValueError("append requires values for all fields")
            columns = {key: [kwargs[key]] for key in self._fields.keys()}
            fields_list = [(key, value) for key, value in self._fields.items()]
            self._net.append_batch(
                self._name,
                fields_list,
                columns,
                database=self._database,
            )
            return
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
            if expected is str and not isinstance(value, str):
                raise TypeError(f"field '{key}' expects string")
            if expected is bytes and not isinstance(value, (bytes, bytearray)):
                raise TypeError(f"field '{key}' expects bytes")
            if expected is int and isinstance(value, bool):
                raise TypeError(f"field '{key}' expects {type_name}")
            if expected is float and isinstance(value, bool):
                raise TypeError(f"field '{key}' expects {type_name}")
            if expected is int:
                value = int(value)
            if expected is float:
                value = float(value)
            if expected is bytes and isinstance(value, bytearray):
                value = bytes(value)
            self._columns[key].append(value)
            self._validity[key].append(True)

    def append_batch(self, columns: dict[str, list]) -> None:
        if self._lock_mode not in ("append-only", "full-crud"):
            raise RuntimeError(f"append_batch not allowed for lock_mode '{self._lock_mode}'")
        if self._net is not None:
            if set(columns.keys()) != set(self._fields.keys()):
                raise ValueError("append_batch requires values for all fields")
            fields_list = [(key, value) for key, value in self._fields.items()]
            self._net.append_batch(
                self._name,
                fields_list,
                columns,
                database=self._database,
            )
            return
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
                if expected is str and not isinstance(value, str):
                    raise TypeError(f"field '{key}' expects string")
                if expected is bytes and not isinstance(value, (bytes, bytearray)):
                    raise TypeError(f"field '{key}' expects bytes")
                if expected is int and isinstance(value, bool):
                    raise TypeError(f"field '{key}' expects {type_name}")
                if expected is float and isinstance(value, bool):
                    raise TypeError(f"field '{key}' expects {type_name}")
                if expected is int:
                    value = int(value)
                if expected is float:
                    value = float(value)
                if expected is bytes and isinstance(value, bytearray):
                    value = bytes(value)
                col_out.append(value)
                valid_out.append(True)
        if count == 0:
            return

    def filter(self, **kwargs) -> Query:
        return Query(self)._with_predicates(kwargs)

    def aggregate(self, **kwargs):
        return Query(self).aggregate(**kwargs)

    def scan(self, columns: list[str] | None = None, limit: int = 0, offset: int = 0, **kwargs):
        query = Query(self)
        if kwargs:
            query = query._with_predicates(kwargs)
        return query.scan(columns=columns, limit=limit, offset=offset)

    def query(self, debug: bool = False, **kwargs):
        query = Query(self)
        if kwargs:
            query = query._with_predicates(kwargs)
        return query.execute(debug=debug)

    def vector_search(self, field: str, query: list[float], top_k: int = 10,
                      metric: str = "cosine",
                      predicates: list[tuple[int, int, float]] | None = None,
                      approximate: bool = False, probes: int = 0,
                      ) -> list[dict[str, int | float]]:
        if field not in self._fields or self._fields[field] != "vector_float32":
            raise ValueError(f"field '{field}' is not vector_float32")
        field_index = list(self._fields).index(field)
        if self._net is not None:
            return self._net.vector_search(self._name, field_index, query, top_k=top_k,
                                           metric=metric, database=self._database,
                                           predicates=predicates, approximate=approximate,
                                           probes=probes)
        if self._cpp is not None:
            string_ops = {0: "eq", 1: "ne", 2: "lt", 3: "le", 4: "gt", 5: "ge"}
            cpp_predicates = [(index, string_ops[op], value)
                              for index, op, value in (predicates or [])]
            return self._cpp.vector_search(field_index, query, top_k, metric, cpp_predicates,
                                           approximate, probes)
        raise RuntimeError("vector_search requires the C++ or network backend")

    def _row_count(self) -> int:
        if self._cpp is not None:
            raise RuntimeError("row_count unavailable for C++ backend")
        if self._net is not None:
            raise RuntimeError("row_count unavailable for network backend")
        if not self._columns:
            return 0
        first = next(iter(self._columns.values()))
        return len(first)

    def compression_stats(self) -> dict | None:
        if self._cpp is not None:
            return self._cpp.compression_stats()
        return None

    def _column(self, name: str) -> list:
        if self._cpp is not None:
            raise RuntimeError("column access unavailable for C++ backend")
        if self._net is not None:
            raise RuntimeError("column access unavailable for network backend")
        return self._columns[name]

    def _valid(self, name: str) -> list[bool]:
        if self._cpp is not None:
            raise RuntimeError("validity access unavailable for C++ backend")
        if self._net is not None:
            raise RuntimeError("validity access unavailable for network backend")
        return self._validity[name]
