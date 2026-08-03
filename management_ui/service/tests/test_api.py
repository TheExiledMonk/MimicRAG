from __future__ import annotations

import struct

from fastapi.testclient import TestClient

import management_ui.service.app as app_module


def write_schema(path, fields):
    path.mkdir(parents=True, exist_ok=True)
    schema_path = path / "schema.bin"
    header = struct.pack("<IIHH", 0x4D435343, 1, len(fields), 0)
    data = bytearray(header)
    for name, type_id in fields:
        name_bytes = name.encode("utf-8")
        data += struct.pack("<H", len(name_bytes))
        data += name_bytes
        data += struct.pack("<B", type_id)
    schema_path.write_bytes(data)


class FakeClient:
    def scan(self, *args, **kwargs):
        return [{"value": 1}]

    def query_agg(self, *args, **kwargs):
        return {"count": 1, "sum": 1.0, "min": 1.0, "max": 1.0}


def test_scan_limit_guardrail(tmp_path, monkeypatch):
    object.__setattr__(app_module.config, "storage_root", str(tmp_path))
    object.__setattr__(app_module.config, "max_scan_limit", 5)
    object.__setattr__(app_module.config, "max_scan_cells", 100)

    write_schema(tmp_path / "default" / "events", [("value", 1)])
    monkeypatch.setattr(app_module.state, "get_client", lambda: FakeClient())

    client = TestClient(app_module.app)
    response = client.post(
        "/api/scan",
        json={
            "database": "default",
            "dataset": "events",
            "columns": ["value"],
            "predicates": [],
            "limit": 10,
            "cursor": None,
        },
    )
    assert response.status_code == 400
    assert "max_scan_limit" in response.json()["detail"]


def test_scan_bool_predicate_op(tmp_path, monkeypatch):
    object.__setattr__(app_module.config, "storage_root", str(tmp_path))
    write_schema(tmp_path / "default" / "flags", [("flag", 3)])
    monkeypatch.setattr(app_module.state, "get_client", lambda: FakeClient())

    client = TestClient(app_module.app)
    response = client.post(
        "/api/scan",
        json={
            "database": "default",
            "dataset": "flags",
            "columns": ["flag"],
            "predicates": [
                {"field": "flag", "op": "gt", "value": True}
            ],
            "limit": 1,
            "cursor": None,
        },
    )
    assert response.status_code == 400
    assert "unsupported op" in response.json()["detail"]


def test_aggregate_rejects_string_field(tmp_path, monkeypatch):
    object.__setattr__(app_module.config, "storage_root", str(tmp_path))
    write_schema(tmp_path / "default" / "names", [("name", 5)])
    monkeypatch.setattr(app_module.state, "get_client", lambda: FakeClient())

    client = TestClient(app_module.app)
    response = client.post(
        "/api/aggregate",
        json={
            "database": "default",
            "dataset": "names",
            "field": "name",
            "predicates": [],
        },
    )
    assert response.status_code == 400
    assert "unsupported aggregate field" in response.json()["detail"]


def test_memory_review_and_action_proxy(monkeypatch):
    calls = []
    monkeypatch.setattr(app_module, "_memory_request", lambda path, payload: calls.append((path, payload)) or {"memories": []})
    client = TestClient(app_module.app)
    assert client.post("/api/memory/review", json={"tenant_id": "acme", "status": "quarantined"}).status_code == 200
    assert client.post("/api/memory/action", json={"tenant_id": "acme", "memory_id": "mem-1", "action": "reject", "reason": "unsafe"}).status_code == 200
    assert calls[0] == ("/v1/memory/review", {"tenant_id": "acme", "status": "quarantined"})
    assert calls[1][0] == "/v1/memory/reject"
    assert client.post("/api/dream/run", json={"tenant_id": "acme", "mode": "deep", "enabled": True}).status_code == 200
    assert client.post("/api/dream/review", json={"tenant_id": "acme", "status": "pending_review"}).status_code == 200
    assert client.post("/api/dream/action", json={"tenant_id": "acme", "refinement_id": "ref-1", "decision": "approved"}).status_code == 200
    assert calls[2][0] == "/v1/dream/run"; assert calls[3][0] == "/v1/dream/review"; assert calls[4][0] == "/v1/dream/action"
