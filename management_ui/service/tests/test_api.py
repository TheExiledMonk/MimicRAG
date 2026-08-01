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
