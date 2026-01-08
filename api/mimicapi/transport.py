from __future__ import annotations

from dataclasses import dataclass

from .adapter import BackendAdapter

from .dataset import Dataset
from .query import _NET_OPS


_OP_TO_SUFFIX = {
    0: "eq",
    1: "ne",
    2: "lt",
    3: "le",
    4: "gt",
    5: "ge",
}


Transport = BackendAdapter


@dataclass
class LocalTransport:
    datasets: dict[tuple[str, str], Dataset] = None  # type: ignore[assignment]
    seen_batches: dict[tuple[str, str], set[int]] = None  # type: ignore[assignment]

    def __post_init__(self) -> None:
        if self.datasets is None:
            self.datasets = {}
        if self.seen_batches is None:
            self.seen_batches = {}

    def create_database(self, name: str) -> None:
        return None

    def list_databases(self) -> list[str]:
        return sorted({db for db, _ in self.datasets.keys()})

    def drop_database(self, name: str) -> None:
        keys = [key for key in self.datasets if key[0] == name]
        for key in keys:
            self.datasets.pop(key, None)
            self.seen_batches.pop(key, None)

    def drop_dataset(self, db: str, name: str) -> None:
        key = (db, name)
        self.datasets.pop(key, None)
        self.seen_batches.pop(key, None)

    def create_dataset(self, db: str, name: str, fields: list[tuple[str, str]]) -> None:
        key = (db, name)
        if key in self.datasets:
            return
        self.datasets[key] = Dataset(name=name, fields=dict(fields), use_cpp=True)

    def append_batch(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, list],
        batch_id: int | None = None,
    ) -> None:
        key = (db, dataset)
        if key not in self.datasets:
            self.create_dataset(db, dataset, fields)
        if batch_id is not None:
            seen = self.seen_batches.setdefault(key, set())
            if batch_id in seen:
                return
            seen.add(batch_id)
        self.datasets[key].append_batch(columns)

    def query_agg(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]:
        key = (db, dataset)
        if key not in self.datasets:
            raise KeyError(f"dataset '{dataset}' not found in db '{db}'")
        data = self.datasets[key]
        fields = list(data.fields.keys())
        if field_index >= len(fields):
            raise IndexError("field_index out of range")
        field_name = fields[field_index]
        query = data.filter()
        field_types = data.fields
        for pred_field, pred_op, pred_value in predicates or []:
            if pred_field >= len(fields):
                raise IndexError("predicate field_index out of range")
            suffix = _OP_TO_SUFFIX[pred_op]
            field_name_pred = fields[pred_field]
            value = _coerce_predicate(field_types[field_name_pred], pred_value)
            query = query.filter(**{f"{field_name_pred}_{suffix}": value})
        return query.aggregate(sum=field_name, count=True, min=field_name, max=field_name)

    def scan(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: list[str] | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]:
        key = (db, dataset)
        if key not in self.datasets:
            raise KeyError(f"dataset '{dataset}' not found in db '{db}'")
        data = self.datasets[key]
        field_names = list(data.fields.keys())
        field_types = data.fields
        query = data.filter()
        for pred_field, pred_op, pred_value in predicates or []:
            if pred_field >= len(field_names):
                raise IndexError("predicate field_index out of range")
            suffix = _OP_TO_SUFFIX[pred_op]
            field_name = field_names[pred_field]
            value = _coerce_predicate(field_types[field_name], pred_value)
            query = query.filter(**{f"{field_name}_{suffix}": value})
        return query.scan(columns=columns, limit=limit, offset=offset)


@dataclass
class NetworkTransport:
    host: str
    port: int
    default_db: str = "default"

    def create_database(self, name: str) -> None:
        client = self._client()
        client.create_database(name)

    def list_databases(self) -> list[str]:
        client = self._client()
        return client.list_databases()

    def drop_database(self, name: str) -> None:
        client = self._client()
        client.drop_database(name)

    def drop_dataset(self, db: str, name: str) -> None:
        client = self._client()
        client.drop_dataset(name, database=db)

    def create_dataset(self, db: str, name: str, fields: list[tuple[str, str]]) -> None:
        client = self._client()
        client.create_database(db)
        client.create_dataset(name, fields, database=db)

    def append_batch(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, list],
        batch_id: int | None = None,
    ) -> None:
        client = self._client()
        client.append_batch(dataset, fields, columns, database=db, batch_id=batch_id)

    def query_agg(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]:
        client = self._client()
        return client.query_agg(
            dataset,
            field_index,
            database=db,
            predicates=predicates,
        )

    def _client(self):
        from client.mimicdb_client import MimicDBClient  # local import to avoid hard dep
        return MimicDBClient(host=self.host, port=self.port, default_db=self.default_db)


def _coerce_predicate(field_type: str, value: float):
    ftype = field_type.lower()
    if ftype in ("int32", "int64"):
        return int(value)
    if ftype == "bool":
        return bool(value)
    return value

    def scan(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: list[str] | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]:
        client = self._client()
        return client.scan(
            dataset,
            fields=fields,
            columns=columns,
            database=db,
            predicates=predicates,
            limit=limit,
            offset=offset,
        )
