import socket
import struct
import time
from typing import Iterable


MAGIC = 0x4D434442
VERSION = 1

OP_PING = 1
OP_CREATE_DATASET = 2
OP_APPEND_BATCH = 3
OP_QUERY_AGG = 4
OP_HEALTH = 5
OP_CREATE_DATABASE = 6
OP_LIST_DATABASES = 7
OP_SCAN = 8
OP_DROP_DATABASE = 9
OP_DROP_DATASET = 10

STATUS_OK = 0

FIELD_TYPES = {
    "int32": 0,
    "int64": 1,
    "float64": 2,
    "bool": 3,
    "dict_int32": 4,
    "string": 5,
    "bytes": 6,
}

TYPE_SIZES = {
    0: 4,
    1: 8,
    2: 8,
    3: 1,
    4: 4,
    5: 0,
    6: 0,
}


class ProtocolError(RuntimeError):
    pass


class MimicDBClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9000,
                 default_db: str = "default") -> None:
        self._host = host
        self._port = port
        self._default_db = default_db
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

    def set_default_db(self, name: str) -> None:
        self._default_db = name

    def ping(self) -> None:
        self._request(OP_PING, b"")

    def create_database(self, name: str) -> None:
        payload = bytearray()
        payload += struct.pack("<H", len(name))
        payload += name.encode("utf-8")
        self._request(OP_CREATE_DATABASE, bytes(payload))

    def list_databases(self) -> list[str]:
        response = self._request(OP_LIST_DATABASES, b"")
        if len(response) < 2:
            raise ProtocolError("short list_databases response")
        count = struct.unpack_from("<H", response, 0)[0]
        cursor = 2
        names = []
        for _ in range(count):
            if cursor + 2 > len(response):
                raise ProtocolError("short list_databases response")
            name_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + name_len > len(response):
                raise ProtocolError("short list_databases response")
            name = response[cursor:cursor + name_len].decode("utf-8")
            cursor += name_len
            names.append(name)
        return names

    def create_dataset(self, name: str, fields: list[tuple[str, str]],
                       database: str | None = None) -> None:
        db_name = self._default_db if database is None else database
        payload = bytearray()
        payload += struct.pack("<H", len(db_name))
        payload += db_name.encode("utf-8")
        payload += struct.pack("<H", len(name))
        payload += name.encode("utf-8")
        payload += struct.pack("<H", len(fields))
        for field_name, field_type in fields:
            type_id = FIELD_TYPES[field_type]
            payload += struct.pack("<H", len(field_name))
            payload += field_name.encode("utf-8")
            payload += struct.pack("<B", type_id)
        self._request(OP_CREATE_DATASET, bytes(payload))

    def drop_database(self, name: str) -> None:
        payload = bytearray()
        payload += struct.pack("<H", len(name))
        payload += name.encode("utf-8")
        self._request(OP_DROP_DATABASE, bytes(payload))

    def drop_dataset(self, name: str, database: str | None = None) -> None:
        db_name = self._default_db if database is None else database
        payload = bytearray()
        payload += struct.pack("<H", len(db_name))
        payload += db_name.encode("utf-8")
        payload += struct.pack("<H", len(name))
        payload += name.encode("utf-8")
        self._request(OP_DROP_DATASET, bytes(payload))

    def append_batch(
        self,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: dict[str, Iterable],
        database: str | None = None,
        batch_id: int | None = None,
    ) -> None:
        db_name = self._default_db if database is None else database
        if batch_id is None:
            batch_id = int(time.time_ns() & 0xFFFFFFFFFFFFFFFF)
        payload = bytearray()
        payload += struct.pack("<H", len(db_name))
        payload += db_name.encode("utf-8")
        payload += struct.pack("<H", len(dataset))
        payload += dataset.encode("utf-8")
        payload += struct.pack("<Q", batch_id)

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
            if field_type in ("string", "bytes"):
                validity_mode, validity_bytes, lengths, data = _pack_varlen(values, field_type)
                packed = lengths + data
                payload += struct.pack("<H", field_index)
                payload += struct.pack("<B", type_id)
                payload += struct.pack("<B", validity_mode)
                payload += struct.pack("<I", row_count)
                payload += struct.pack("<I", len(data))
                payload += packed
                if validity_mode == 1:
                    payload += validity_bytes
                continue
            validity_mode, validity_bytes, packed = _pack_values(values, field_type)
            payload += struct.pack("<H", field_index)
            payload += struct.pack("<B", type_id)
            payload += struct.pack("<B", validity_mode)
            payload += struct.pack("<I", row_count)
            payload += packed
            if validity_mode == 1:
                payload += validity_bytes

        self._request(OP_APPEND_BATCH, bytes(payload))

    def query_agg(
        self,
        dataset: str,
        field_index: int,
        database: str | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> dict[str, float]:
        db_name = self._default_db if database is None else database
        payload = bytearray()
        payload += struct.pack("<H", len(db_name))
        payload += db_name.encode("utf-8")
        payload += struct.pack("<H", len(dataset))
        payload += dataset.encode("utf-8")
        payload += struct.pack("<H", field_index)
        pred_list = predicates or []
        payload += struct.pack("<H", len(pred_list))
        for pred_field, pred_op, pred_value in pred_list:
            payload += struct.pack("<H", pred_field)
            payload += struct.pack("<B", pred_op)
            payload += struct.pack("<d", float(pred_value))
        response = self._request(OP_QUERY_AGG, bytes(payload))
        if len(response) < 8 + 8 * 3 + 1 + 8:
            raise ProtocolError("short aggregate response")
        count, sum_val, min_val, max_val = struct.unpack_from("<Qddd", response, 0)
        has_value = response[32] == 1
        rows_scanned = struct.unpack_from("<Q", response, 33)[0]
        return {
            "count": count,
            "sum": sum_val,
            "min": min_val if has_value else None,
            "max": max_val if has_value else None,
            "rows_scanned": rows_scanned,
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

    def scan(
        self,
        dataset: str,
        fields: list[tuple[str, str]],
        columns: list[str] | None = None,
        database: str | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]:
        db_name = self._default_db if database is None else database
        if columns is None:
            columns = [name for name, _ in fields]
        field_index = {name: i for i, (name, _) in enumerate(fields)}
        column_indices = [field_index[name] for name in columns]

        payload = bytearray()
        payload += struct.pack("<H", len(db_name))
        payload += db_name.encode("utf-8")
        payload += struct.pack("<H", len(dataset))
        payload += dataset.encode("utf-8")
        payload += struct.pack("<H", len(column_indices))
        for idx in column_indices:
            payload += struct.pack("<H", idx)
        pred_list = predicates or []
        payload += struct.pack("<H", len(pred_list))
        for pred_field, pred_op, pred_value in pred_list:
            payload += struct.pack("<H", pred_field)
            payload += struct.pack("<B", pred_op)
            payload += struct.pack("<d", float(pred_value))
        payload += struct.pack("<Q", int(limit))
        payload += struct.pack("<Q", int(offset))

        response = self._request(OP_SCAN, bytes(payload))
        cursor = 0
        if len(response) < 6:
            raise ProtocolError("short scan response")
        row_count = struct.unpack_from("<I", response, cursor)[0]
        cursor += 4
        field_count = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        columns_data: dict[int, list] = {}
        columns_validity: dict[int, list[bool]] = {}
        for _ in range(field_count):
            if cursor + 8 > len(response):
                raise ProtocolError("short scan response")
            field_idx = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            field_type = response[cursor]
            cursor += 1
            validity_mode = response[cursor]
            cursor += 1
            element_count = struct.unpack_from("<I", response, cursor)[0]
            cursor += 4
            if element_count != row_count:
                raise ProtocolError("scan response row mismatch")
            varlen = field_type in (5, 6)
            data_bytes = 0
            lengths = None
            if varlen:
                if cursor + 4 > len(response):
                    raise ProtocolError("short scan response")
                data_bytes = struct.unpack_from("<I", response, cursor)[0]
                cursor += 4
                lengths_bytes = element_count * 4
                if cursor + lengths_bytes + data_bytes > len(response):
                    raise ProtocolError("short scan response")
                length_buf = response[cursor:cursor + lengths_bytes]
                cursor += lengths_bytes
                lengths = list(struct.unpack_from(f"<{element_count}I", length_buf, 0))
            else:
                type_size = TYPE_SIZES[field_type]
                data_bytes = element_count * type_size
            if cursor + data_bytes > len(response):
                raise ProtocolError("short scan response")
            data = response[cursor:cursor + data_bytes]
            cursor += data_bytes
            validity = [True] * element_count
            if validity_mode == 1:
                validity_bytes = (element_count + 7) // 8
                if cursor + validity_bytes > len(response):
                    raise ProtocolError("short scan response")
                bitmap = response[cursor:cursor + validity_bytes]
                cursor += validity_bytes
                validity = []
                for i in range(element_count):
                    byte = bitmap[i // 8]
                    bit = (byte >> (i % 8)) & 1
                    validity.append(bit == 1)
            elif validity_mode != 0:
                raise ProtocolError("invalid scan validity mode")
            values = []
            if varlen:
                if lengths is None:
                    raise ProtocolError("missing lengths for varlen field")
                offset_bytes = 0
                for i in range(element_count):
                    length = lengths[i]
                    chunk = data[offset_bytes:offset_bytes + length]
                    offset_bytes += length
                    if field_type == 5:
                        values.append(chunk.decode("utf-8"))
                    else:
                        values.append(chunk)
            else:
                type_size = TYPE_SIZES[field_type]
                for i in range(element_count):
                    offset_bytes = i * type_size
                    if field_type == 0 or field_type == 4:
                        val = struct.unpack_from("<i", data, offset_bytes)[0]
                    elif field_type == 1:
                        val = struct.unpack_from("<q", data, offset_bytes)[0]
                    elif field_type == 2:
                        val = struct.unpack_from("<d", data, offset_bytes)[0]
                    elif field_type == 3:
                        val = data[offset_bytes] != 0
                    else:
                        raise ProtocolError("unknown field type")
                    values.append(val)
            columns_data[field_idx] = values
            columns_validity[field_idx] = validity

        rows = []
        for i in range(row_count):
            row = {}
            for name in columns:
                idx = field_index[name]
                if not columns_validity[idx][i]:
                    row[name] = None
                else:
                    row[name] = columns_data[idx][i]
            rows.append(row)
        return rows

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


def _pack_varlen(values: list, field_type: str) -> tuple[int, bytes, bytes, bytes]:
    lengths = bytearray()
    data = bytearray()
    validity = []
    for value in values:
        if value is None:
            validity.append(0)
            lengths += struct.pack("<I", 0)
            continue
        validity.append(1)
        if field_type == "string":
            if not isinstance(value, str):
                raise ValueError("string field requires str values")
            encoded = value.encode("utf-8")
            lengths += struct.pack("<I", len(encoded))
            data += encoded
        elif field_type == "bytes":
            if isinstance(value, str):
                raise ValueError("bytes field requires bytes values")
            if not isinstance(value, (bytes, bytearray)):
                raise ValueError("bytes field requires bytes values")
            lengths += struct.pack("<I", len(value))
            data += bytes(value)
        else:
            raise ValueError(f"unsupported field type {field_type}")
    if all(validity):
        return 0, b"", bytes(lengths), bytes(data)
    return 1, _pack_validity(validity), bytes(lengths), bytes(data)


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


__all__ = ["MimicDBClient", "ProtocolError"]
