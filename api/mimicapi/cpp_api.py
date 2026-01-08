from __future__ import annotations

try:
    from . import _mimicapi_core
except ImportError:  # pragma: no cover - optional extension
    _mimicapi_core = None


class CppApiClient:
    def __init__(self) -> None:
        if _mimicapi_core is None:
            raise RuntimeError("mimicapi core extension is not available")
        self._core = _mimicapi_core.ApiClientCore()

    def create_database(self, name: str) -> None:
        self._core.create_database(name)

    def create_dataset(self, db: str, name: str, fields: dict[str, str]) -> None:
        self._core.create_dataset(db, name, fields)

    def append_batch(self, db: str, name: str, columns: dict[str, list]) -> None:
        self._core.append_batch(db, name, columns)

    def scan(
        self,
        db: str,
        name: str,
        columns: list[str] | None = None,
        predicates: list[tuple[int, str, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]:
        return self._core.scan(
            db,
            name,
            columns or None,
            predicates or None,
            limit,
            offset,
        )

    def aggregate(
        self,
        db: str,
        name: str,
        field_index: int,
        predicates: list[tuple[int, str, float]] | None = None,
    ) -> dict:
        return self._core.aggregate(db, name, field_index, predicates or None)
