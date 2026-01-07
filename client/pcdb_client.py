import socket
import struct
from typing import Iterable


MAGIC = 0x50434442
VERSION = 1

OP_PING = 1
OP_CREATE_DATASET = 2
OP_APPEND_BATCH = 3
OP_QUERY_AGG = 4
OP_HEALTH = 5

STATUS_OK = 0

FIELD_TYPES = {
    "int32": 0,
    "int64": 1,
    "float64": 2,
    "bool": 3,
    "dict_int32": 4,
}


class ProtocolError(RuntimeError):
    pass


class PCDBClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9000) -> None:
        self._host = host
        self._port = port
        self._sock: socket.socket | None = None
        self._next_id = 1

    def connect(self) -> None:
        if self._sock is not None:
            return
        self._sock = socket.create_connection((self._host, self._port))

    def close(self) -> None:
        if self._sock is None:
            return
        self._sock.close()
        self._sock = None

    def ping(self) -> None:
        self._request(OP_PING, b"")

    def create_dataset(self, name: str, fields: list[tuple[str, str]]) -> None:
        payload = bytearray()
        payload += struct.pack("<H", len(name))
        payload += name.encode("utf-8")
        payload += struct.pack("<H", len(fields))
        for field_name, field_type in fields:
            type_id = FIELD_TYPES[field_type]
            payload += struct.pack("<H", len(field_name))
            payload += field_name.encode("utf-8")
            payload += struct.pack("<B", type_id)
        self._request(OP_CREATE_DATASET, bytes(payload))

    def append_batch(
        self,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, Iterable],
    ) -> None:
        payload = bytearray()
        payload += struct.pack("<H", len(dataset))
        payload += dataset.encode("utf-8")

        field_count = len(fields)
        lengths = {len(list(columns[name])) for name, _ in fields}
        if len(lengths) != 1:
            raise ValueError("all columns must have the same length")
        row_count = lengths.pop()
        payload += struct.pack("<I", row_count)
        payload += struct.pack("<H", field_count)

        for field_index, (name, field_type) in enumerate(fields):
            values = list(columns[name])
            type_id = FIELD_TYPES[field_type]
            validity_mode, validity_bytes, packed = _pack_values(values, field_type)
            payload += struct.pack("<H", field_index)
            payload += struct.pack("<B", type_id)
            payload += struct.pack("<B", validity_mode)
            payload += struct.pack("<I", row_count)
            payload += packed
            if validity_mode == 1:
                payload += validity_bytes

        self._request(OP_APPEND_BATCH, bytes(payload))

    def query_agg(self, dataset: str, field_index: int) -> dict[str, float]:
        payload = bytearray()
        payload += struct.pack("<H", len(dataset))
        payload += dataset.encode("utf-8")
        payload += struct.pack("<H", field_index)
        response = self._request(OP_QUERY_AGG, bytes(payload))
        if len(response) < 8 + 8 * 3 + 1:
            raise ProtocolError("short aggregate response")
        count, sum_val, min_val, max_val = struct.unpack_from("<Qddd", response, 0)
        has_value = response[32] == 1
        return {
            "count": count,
            "sum": sum_val,
            "min": min_val if has_value else None,
            "max": max_val if has_value else None,
        }

    def health(self) -> dict[str, int]:
        response = self._request(OP_HEALTH, b"")
        if len(response) < 2 + 8 + 8:
            raise ProtocolError("short health response")
        dataset_count = struct.unpack_from("<H", response, 0)[0]
        segment_count = struct.unpack_from("<Q", response, 2)[0]
        row_count = struct.unpack_from("<Q", response, 10)[0]
        return {
            "datasets": dataset_count,
            "segments": segment_count,
            "rows": row_count,
        }
    def _request(self, opcode: int, payload: bytes) -> bytes:
        if self._sock is None:
            self.connect()
        request_id = self._next_id
        self._next_id += 1
        header = struct.pack(
            "<IHHHHII",
            MAGIC,
            VERSION,
            0,
            opcode,
            0,
            len(payload),
            request_id,
        )
        assert self._sock is not None
        _send_all(self._sock, header)
        if payload:
            _send_all(self._sock, payload)
        resp_header = _recv_exact(self._sock, 20)
        magic, version, flags, resp_opcode, status, size, resp_id = struct.unpack(
            "<IHHHHII", resp_header
        )
        if magic != MAGIC or version != VERSION:
            raise ProtocolError("invalid response header")
        if resp_opcode != opcode or resp_id != request_id:
            raise ProtocolError("mismatched response")
        payload = _recv_exact(self._sock, size) if size else b""
        if status != STATUS_OK:
            raise ProtocolError(f"server error status={status}")
        return payload


def _pack_values(values: list, field_type: str) -> tuple[int, bytes, bytes]:
    validity = []
    packed = bytearray()
    for value in values:
        if value is None:
            validity.append(0)
            value = 0
        else:
            validity.append(1)
        if field_type == "int32" or field_type == "dict_int32":
            packed += struct.pack("<i", int(value))
        elif field_type == "int64":
            packed += struct.pack("<q", int(value))
        elif field_type == "float64":
            packed += struct.pack("<d", float(value))
        elif field_type == "bool":
            packed += struct.pack("<B", 1 if value else 0)
        else:
            raise ValueError(f"unsupported field type {field_type}")
    if all(validity):
        return 0, b"", bytes(packed)
    validity_bytes = _pack_validity(validity)
    return 1, validity_bytes, bytes(packed)


def _pack_validity(bits: list[int]) -> bytes:
    out = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        if bit:
            out[i // 8] |= 1 << (i % 8)
    return bytes(out)


def _send_all(sock: socket.socket, data: bytes) -> None:
    view = memoryview(data)
    while view:
        sent = sock.send(view)
        view = view[sent:]


def _recv_exact(sock: socket.socket, size: int) -> bytes:
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ProtocolError("connection closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


__all__ = ["PCDBClient", "ProtocolError"]
