from __future__ import annotations

from dataclasses import asdict
import logging
import os
import re
from pathlib import Path
import secrets
import sys
import threading
import time
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.responses import RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import __version__
from .config import ServiceConfig


def _load_client_module():
    try:
        from client.mimicdb_client import MimicDBClient, ProtocolError  # type: ignore
        return MimicDBClient, ProtocolError
    except Exception:
        repo_root = Path(__file__).resolve().parents[2]
        if str(repo_root) not in sys.path:
            sys.path.insert(0, str(repo_root))
        from client.mimicdb_client import MimicDBClient, ProtocolError  # type: ignore
        return MimicDBClient, ProtocolError


class ConnectRequest(BaseModel):
    host: str = Field(default="127.0.0.1")
    port: int = Field(default=9000, ge=1, le=65535)
    database: str = Field(default="default")
    identity_key_path: str | None = None


class ScanPredicate(BaseModel):
    field: str
    op: str
    value: float | int | bool


class ScanRequest(BaseModel):
    database: str
    dataset: str
    columns: list[str] | None = None
    predicates: list[ScanPredicate] = Field(default_factory=list)
    limit: int = Field(default=100, ge=1)
    cursor: str | None = None


class AggregateRequest(BaseModel):
    database: str
    dataset: str
    field: str
    predicates: list[ScanPredicate] = Field(default_factory=list)


class CreateDatabaseRequest(BaseModel):
    name: str


class DropDatabaseRequest(BaseModel):
    name: str


class FieldSpec(BaseModel):
    name: str
    type: str


class CreateDatasetRequest(BaseModel):
    database: str
    name: str
    fields: list[FieldSpec]


class DropDatasetRequest(BaseModel):
    database: str
    name: str


class AppendRequest(BaseModel):
    database: str
    dataset: str
    columns: dict[str, list]


class ConnectionState:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._io_lock = threading.Lock()
        self._client = None
        self._info: dict[str, Any] | None = None

    def connect(
        self,
        host: str,
        port: int,
        database: str,
        identity_key_path: str | None,
    ) -> None:
        MimicDBClient, ProtocolError = _load_client_module()
        key_path = (identity_key_path or config.identity_key_path or "").strip()
        if not key_path:
            raise ClientError({"status": 400, "detail": "identity key path is required"})
        key_file = Path(key_path).expanduser()
        if not key_file.exists():
            raise ClientError(
                {"status": 400, "detail": f"identity key not found: {key_file}"}
            )
        client = MimicDBClient(
            host=host,
            port=port,
            default_db=database,
            identity_key_path=str(key_file),
        )
        try:
            with self._io_lock:
                client.connect()
                client.ping()
        except (OSError, ProtocolError, RuntimeError) as exc:
            raise ClientError(_map_exception(exc)) from exc
        with self._lock:
            if self._client is not None:
                self._client.close()
            self._client = client
            self._info = {
                "host": host,
                "port": port,
                "database": database,
                "identity_key_path": key_path or None,
            }

    def get_client(self):
        with self._lock:
            if self._client is None:
                raise ClientError({"status": 409, "detail": "not connected"})
            return self._client

    def perform(self, action):
        with self._io_lock:
            client = self.get_client()
            return action(client)

    def info(self) -> dict[str, Any] | None:
        with self._lock:
            return None if self._info is None else dict(self._info)


class CursorStore:
    def __init__(self, max_entries: int = 1024) -> None:
        self._lock = threading.Lock()
        self._max_entries = max_entries
        self._entries: dict[str, dict[str, Any]] = {}
        self._order: list[str] = []

    def create(self, state: dict[str, Any]) -> str:
        token = secrets.token_urlsafe(16)
        with self._lock:
            self._entries[token] = state
            self._order.append(token)
            if len(self._order) > self._max_entries:
                oldest = self._order.pop(0)
                self._entries.pop(oldest, None)
        return token

    def get(self, token: str) -> dict[str, Any] | None:
        with self._lock:
            return self._entries.get(token)


config = ServiceConfig()
state = ConnectionState()
cursor_store = CursorStore()
app = FastAPI(title="MimicDB UI Service", version=__version__)

logger = logging.getLogger("mimicdb_ui_service")
if not logger.handlers:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


FIELD_TYPE_NAMES = {
    0: "int32",
    1: "int64",
    2: "float64",
    3: "bool",
    4: "dict_int32",
    5: "string",
    6: "bytes",
    7: "array",
    8: "object",
}

FIELD_TYPE_IDS = {name: type_id for type_id, name in FIELD_TYPE_NAMES.items()}

PREDICATE_OPS = {
    "eq": 0,
    "ne": 1,
    "lt": 2,
    "le": 3,
    "gt": 4,
    "ge": 5,
}
BOOL_ONLY_OPS = {"eq", "ne"}
NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")


def _dataset_path(database: str, dataset: str) -> Path:
    return Path(config.storage_root) / database / dataset


def _read_schema(database: str, dataset: str) -> list[dict[str, Any]]:
    schema_path = _dataset_path(database, dataset) / "schema.bin"
    if not schema_path.exists():
        raise FileNotFoundError(f"schema not found for {database}.{dataset}")
    data = schema_path.read_bytes()
    if len(data) < 12:
        raise ValueError("invalid schema header")
    magic = int.from_bytes(data[0:4], "little")
    version = int.from_bytes(data[4:8], "little")
    field_count = int.from_bytes(data[8:10], "little")
    if magic != 0x4D435343 or version != 1:
        raise ValueError("invalid schema header")
    cursor = 12
    fields = []
    for _ in range(field_count):
        if cursor + 2 > len(data):
            raise ValueError("truncated schema")
        name_len = int.from_bytes(data[cursor:cursor + 2], "little")
        cursor += 2
        if cursor + name_len + 1 > len(data):
            raise ValueError("truncated schema")
        name = data[cursor:cursor + name_len].decode("utf-8")
        cursor += name_len
        field_type = data[cursor]
        cursor += 1
        type_name = FIELD_TYPE_NAMES.get(field_type, "unknown")
        encoding = "dict_int32" if type_name == "dict_int32" else "raw"
        fields.append(
            {
                "name": name,
                "type": type_name,
                "nullable": True,
                "encoding": encoding,
            }
        )
    return fields


def _schema_fields(database: str, dataset: str) -> list[tuple[str, str]]:
    fields = _read_schema(database, dataset)
    return [(field["name"], field["type"]) for field in fields]


def _validate_identifier(value: str, label: str) -> str:
    name = value.strip()
    if not name:
        raise HTTPException(status_code=400, detail=f"{label} cannot be empty")
    if "/" in name or "\\" in name:
        raise HTTPException(status_code=400, detail=f"{label} cannot contain path separators")
    if not NAME_RE.match(name):
        raise HTTPException(
            status_code=400,
            detail=f"{label} must match [A-Za-z0-9_]+",
        )
    return name


def _mount_ui(app_instance: FastAPI) -> None:
    ui_override = os.getenv("MIMICDB_UI_DIST", "").strip()
    if ui_override:
        ui_root = Path(ui_override).expanduser()
    elif getattr(sys, "frozen", False):
        ui_root = Path(getattr(sys, "_MEIPASS", "")) / "ui_dist"
    else:
        ui_root = Path(__file__).resolve().parents[1] / "ui" / "dist"
    if not ui_root.exists():
        return
    app_instance.mount(
        "/ui",
        StaticFiles(directory=ui_root, html=True),
        name="ui",
    )

    @app_instance.get("/")
    def ui_root_redirect():
        return RedirectResponse(url="/ui")


_mount_ui(app)


class ClientError(RuntimeError):
    def __init__(self, payload: dict[str, Any]) -> None:
        super().__init__(payload.get("detail", "client error"))
        self.payload = payload


def _map_exception(exc: Exception) -> dict[str, Any]:
    _, ProtocolError = _load_client_module()
    if isinstance(exc, TimeoutError):
        return {"status": 504, "detail": "network timeout"}
    if isinstance(exc, OSError):
        return {"status": 503, "detail": f"network error: {exc}"}
    if isinstance(exc, ProtocolError):
        return {"status": 502, "detail": f"protocol error: {exc}"}
    return {"status": 503, "detail": str(exc)}


@app.middleware("http")
async def log_requests(request, call_next):
    start = time.perf_counter()
    response = await call_next(request)
    duration_ms = (time.perf_counter() - start) * 1000
    logger.info(
        "request method=%s path=%s status=%s duration_ms=%.1f",
        request.method,
        request.url.path,
        response.status_code,
        duration_ms,
    )
    if request.url.path.startswith("/ui"):
        response.headers["Cache-Control"] = "no-store, max-age=0"
    return response


def _validate_predicates(
    predicates_in: list[ScanPredicate],
    field_index: dict[str, int],
    fields: list[tuple[str, str]],
) -> list[tuple[int, int, float]]:
    predicates: list[tuple[int, int, float]] = []
    for predicate in predicates_in:
        op_code = PREDICATE_OPS.get(predicate.op)
        if op_code is None:
            raise HTTPException(status_code=400, detail=f"unsupported op: {predicate.op}")
        if predicate.field not in field_index:
            raise HTTPException(status_code=400, detail=f"unknown field: {predicate.field}")
        field_type = fields[field_index[predicate.field]][1]
        if field_type in ("string", "bytes", "array", "object"):
            raise HTTPException(
                status_code=400,
                detail=f"unsupported predicate field: {predicate.field}",
            )
        value = predicate.value
        if field_type == "bool":
            if not isinstance(value, bool):
                raise HTTPException(
                    status_code=400,
                    detail=f"predicate value must be bool for {predicate.field}",
                )
            if predicate.op not in BOOL_ONLY_OPS:
                raise HTTPException(
                    status_code=400,
                    detail=f"unsupported op for bool field: {predicate.op}",
                )
            numeric_value = 1.0 if value else 0.0
        elif field_type in ("int32", "int64", "dict_int32"):
            if not isinstance(value, int) or isinstance(value, bool):
                raise HTTPException(
                    status_code=400,
                    detail=f"predicate value must be int for {predicate.field}",
                )
            numeric_value = float(value)
        else:
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                raise HTTPException(
                    status_code=400,
                    detail=f"predicate value must be numeric for {predicate.field}",
                )
            numeric_value = float(value)
        predicates.append((field_index[predicate.field], op_code, numeric_value))
    return predicates


def _validate_aggregate_field(field_name: str, fields: list[tuple[str, str]]) -> int:
    field_index = {name: idx for idx, (name, _) in enumerate(fields)}
    if field_name not in field_index:
        raise HTTPException(status_code=400, detail=f"unknown field: {field_name}")
    field_type = fields[field_index[field_name]][1]
    if field_type in ("string", "bytes", "array", "object", "bool"):
        raise HTTPException(
            status_code=400,
            detail=f"unsupported aggregate field: {field_name}",
        )
    return field_index[field_name]


@app.get("/api/health")
def health() -> dict[str, Any]:
    return {"ok": True, "version": __version__, "config": asdict(config)}


@app.post("/api/connect")
def connect(payload: ConnectRequest) -> dict[str, Any]:
    try:
        state.connect(
            payload.host,
            payload.port,
            payload.database,
            payload.identity_key_path,
        )
    except ClientError as exc:
        raise HTTPException(status_code=exc.payload["status"], detail=exc.payload["detail"]) from exc
    return {"ok": True}


@app.get("/api/databases")
def list_databases() -> dict[str, Any]:
    try:
        databases = state.perform(lambda client: client.list_databases())
    except ClientError as exc:
        raise HTTPException(status_code=exc.payload["status"], detail=exc.payload["detail"]) from exc
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"databases": databases}


@app.get("/api/datasets")
def list_datasets(database: str) -> dict[str, Any]:
    base = Path(config.storage_root) / database
    if not base.exists():
        raise HTTPException(status_code=404, detail="database not found on disk")
    datasets = sorted(
        entry.name
        for entry in base.iterdir()
        if entry.is_dir()
    )
    return {"datasets": datasets}


@app.get("/api/schema")
def get_schema(database: str, dataset: str) -> dict[str, Any]:
    try:
        fields = _read_schema(database, dataset)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return {"fields": fields}


@app.post("/api/scan")
def scan(payload: ScanRequest) -> dict[str, Any]:
    if payload.limit > config.max_scan_limit:
        raise HTTPException(
            status_code=400,
            detail=f"limit exceeds max_scan_limit ({config.max_scan_limit})",
        )
    if payload.cursor is not None:
        state_entry = cursor_store.get(payload.cursor)
        if state_entry is None:
            raise HTTPException(status_code=404, detail="cursor not found")
        expected = {
            "database": payload.database,
            "dataset": payload.dataset,
            "columns": payload.columns,
            "predicates": [pred.dict() for pred in payload.predicates],
            "limit": payload.limit,
        }
        if expected != state_entry["query"]:
            raise HTTPException(status_code=409, detail="cursor query mismatch")
        offset = state_entry["offset"]
    else:
        offset = 0

    try:
        client = state.get_client()
    except ClientError as exc:
        raise HTTPException(status_code=exc.payload["status"], detail=exc.payload["detail"]) from exc

    try:
        schema_fields = _read_schema(payload.database, payload.dataset)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    fields = [(field["name"], field["type"]) for field in schema_fields]
    field_index = {name: idx for idx, (name, _) in enumerate(fields)}

    columns = payload.columns
    if columns is not None:
        missing = [name for name in columns if name not in field_index]
        if missing:
            raise HTTPException(status_code=400, detail=f"unknown columns: {missing}")

    predicates = _validate_predicates(payload.predicates, field_index, fields)

    column_count = len(columns) if columns is not None else len(fields)
    max_rows = config.max_scan_cells // max(column_count, 1)
    if payload.limit > max_rows:
        raise HTTPException(
            status_code=400,
            detail=f"limit too large for payload cap (max {max_rows} rows)",
        )

    fetch_limit = payload.limit + 1
    try:
        rows = state.perform(
            lambda client: client.scan(
                payload.dataset,
                fields,
                columns=columns,
                database=payload.database,
                predicates=predicates,
                limit=fetch_limit,
                offset=offset,
            )
        )
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc

    has_more = len(rows) > payload.limit
    if has_more:
        rows = rows[:payload.limit]

    next_cursor = None
    if has_more:
        query_state = {
            "database": payload.database,
            "dataset": payload.dataset,
            "columns": payload.columns,
            "predicates": [pred.dict() for pred in payload.predicates],
            "limit": payload.limit,
        }
        next_cursor = cursor_store.create(
            {"query": query_state, "offset": offset + payload.limit}
        )

    columns_out = columns if columns is not None else [name for name, _ in fields]
    rows_out = [[row.get(name) for name in columns_out] for row in rows]

    return {
        "columns": columns_out,
        "rows": rows_out,
        "cursor": next_cursor,
        "has_more": has_more,
    }


@app.post("/api/aggregate")
def aggregate(payload: AggregateRequest) -> dict[str, Any]:
    try:
        client = state.get_client()
    except ClientError as exc:
        raise HTTPException(status_code=exc.payload["status"], detail=exc.payload["detail"]) from exc

    try:
        fields = _schema_fields(payload.database, payload.dataset)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    field_index_map = {name: idx for idx, (name, _) in enumerate(fields)}
    field_index = _validate_aggregate_field(payload.field, fields)
    predicates = _validate_predicates(payload.predicates, field_index_map, fields)

    try:
        result = state.perform(
            lambda client: client.query_agg(
                payload.dataset,
                field_index,
                database=payload.database,
                predicates=predicates,
            )
        )
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc

    has_value = result.get("min") is not None
    return {
        "count": result["count"],
        "sum": result["sum"],
        "min": result["min"],
        "max": result["max"],
        "has_value": has_value,
    }


@app.post("/api/create_database")
def create_database(payload: CreateDatabaseRequest) -> dict[str, Any]:
    try:
        name = _validate_identifier(payload.name, "database name")
        state.perform(lambda client: client.create_database(name))
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"ok": True}


@app.post("/api/drop_database")
def drop_database(payload: DropDatabaseRequest) -> dict[str, Any]:
    try:
        name = _validate_identifier(payload.name, "database name")
        state.perform(lambda client: client.drop_database(name))
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"ok": True}


@app.post("/api/create_dataset")
def create_dataset(payload: CreateDatasetRequest) -> dict[str, Any]:
    missing = [field.type for field in payload.fields if field.type not in FIELD_TYPE_IDS]
    if missing:
        raise HTTPException(status_code=400, detail=f"unsupported field types: {missing}")
    database = _validate_identifier(payload.database, "database name")
    dataset = _validate_identifier(payload.name, "dataset name")
    fields = [
        (_validate_identifier(field.name, "field name"), field.type)
        for field in payload.fields
    ]
    try:
        state.perform(lambda client: client.create_dataset(dataset, fields, database=database))
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"ok": True}


@app.post("/api/drop_dataset")
def drop_dataset(payload: DropDatasetRequest) -> dict[str, Any]:
    try:
        database = _validate_identifier(payload.database, "database name")
        dataset = _validate_identifier(payload.name, "dataset name")
        state.perform(lambda client: client.drop_dataset(dataset, database=database))
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"ok": True}


@app.post("/api/append")
def append_batch(payload: AppendRequest) -> dict[str, Any]:
    try:
        fields = _schema_fields(payload.database, payload.dataset)
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc

    field_names = [name for name, _ in fields]
    if set(payload.columns.keys()) != set(field_names):
        raise HTTPException(status_code=400, detail="append requires values for all fields")

    try:
        state.perform(
            lambda client: client.append_batch(
                payload.dataset,
                fields,
                payload.columns,
                database=payload.database,
            )
        )
    except Exception as exc:
        error = _map_exception(exc)
        raise HTTPException(status_code=error["status"], detail=error["detail"]) from exc
    return {"ok": True}
