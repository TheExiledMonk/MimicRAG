from __future__ import annotations

from .adapter import BackendAdapter
from .transport import LocalTransport, NetworkTransport


class MimicDBAdapter(BackendAdapter):
    def __init__(
        self,
        host: str | None = None,
        port: int | None = None,
        default_db: str = "default",
    ) -> None:
        if host is None and port is None:
            self._transport: BackendAdapter = LocalTransport()
        else:
            self._transport = NetworkTransport(
                host=host or "127.0.0.1",
                port=port or 9000,
                default_db=default_db,
            )

    def create_database(self, name: str) -> None:
        self._transport.create_database(name)

    def list_databases(self) -> list[str]:
        return self._transport.list_databases()

    def create_dataset(self, db: str, name: str, fields: list[tuple[str, str]]) -> None:
        self._transport.create_dataset(db, name, fields)

    def append_batch(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, list],
        batch_id: int | None = None,
    ) -> None:
        self._transport.append_batch(db, dataset, fields, columns, batch_id=batch_id)

    def query_agg(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]:
        return self._transport.query_agg(db, dataset, field_index, predicates)

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
        return self._transport.scan(db, dataset, fields, columns, predicates, limit, offset)
