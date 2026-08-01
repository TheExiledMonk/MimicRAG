from __future__ import annotations

try:
    from . import _mimicapi_core
except ImportError as exc:  # pragma: no cover - optional extension
    _mimicapi_core = None
    _mimicapi_core_error = str(exc)
else:
    _mimicapi_core_error = None


class CppApiClient:
    def __init__(self) -> None:
        if _mimicapi_core is None:
            raise RuntimeError("mimicapi core extension is not available")
        self._core = _mimicapi_core.ApiClientCore()

    def create_database(self, name: str) -> None:
        self._core.create_database(name)

    def create_dataset(self, db: str, name: str, fields: dict[str, str]) -> None:
        self._core.create_dataset(db, name, fields)

    def drop_database(self, name: str) -> None:
        self._core.drop_database(name)

    def drop_dataset(self, db: str, name: str) -> None:
        self._core.drop_dataset(db, name)

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
        try:
            return self._core.scan(
                db,
                name,
                columns or None,
                predicates or None,
                limit,
                offset,
            )
        except KeyError as exc:
            raise RuntimeError(str(exc)) from exc
        except RuntimeError as exc:
            message = str(exc)
            if "invalid predicate" in message:
                raise TypeError(message) from exc
            raise

    def scan_debug(
        self,
        db: str,
        name: str,
        columns: list[str] | None = None,
        predicates: list[tuple[int, str, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> tuple[list[dict], dict]:
        try:
            return self._core.scan_debug(
                db,
                name,
                columns or None,
                predicates or None,
                limit,
                offset,
            )
        except KeyError as exc:
            raise RuntimeError(str(exc)) from exc
        except RuntimeError as exc:
            message = str(exc)
            if "invalid predicate" in message:
                raise TypeError(message) from exc
            raise

    def aggregate(
        self,
        db: str,
        name: str,
        field_index: int,
        predicates: list[tuple[int, str, float]] | None = None,
    ) -> dict:
        return self._core.aggregate(db, name, field_index, predicates or None)

    def aggregate_multi(
        self,
        db: str,
        name: str,
        requests: list[dict],
        predicates: list[tuple[int, str, float]] | None = None,
    ) -> dict:
        return self._core.aggregate_multi(db, name, requests, predicates or None)

    def compression_stats(self, db: str, name: str) -> dict:
        return self._core.compression_stats(db, name)

    def set_compression_enabled(self, enabled: bool) -> None:
        _mimicapi_core.set_compression_enabled(bool(enabled))
