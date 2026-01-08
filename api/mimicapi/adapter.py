from __future__ import annotations

from typing import Protocol


class BackendAdapter(Protocol):
    def create_database(self, name: str) -> None: ...
    def list_databases(self) -> list[str]: ...
    def create_dataset(self, db: str, name: str, fields: list[tuple[str, str]]) -> None: ...
    def append_batch(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, list],
        batch_id: int | None = None,
    ) -> None: ...
    def query_agg(
        self,
        db: str,
        dataset: str,
        field_index: int,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]: ...
    def scan(
        self,
        db: str,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: list[str] | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]: ...
