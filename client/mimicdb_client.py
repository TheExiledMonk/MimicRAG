import hashlib
import os
from pathlib import Path
import socket
import struct
import time
from typing import Iterable

from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes, serialization


MAGIC = 0x4D434442
VERSION = 1
SEC_MAGIC = 0x4D534543
SEC_VERSION = 1
CIPHER_CHACHA20_POLY1305 = 1
FLAG_SESSION_ID = 0x2

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
OP_HOST_KEY = 11
OP_HOST_KEY_ROTATE = 12
OP_VECTOR_SEARCH = 13
OP_AUTH_INIT_ROOT = 100
OP_AUTH_KEY_ADD = 101
OP_AUTH_KEY_DISABLE = 102
OP_AUTH_KEY_REMOVE = 103
OP_AUTH_KEY_LIST = 104
OP_AUTH_ROLE_CREATE = 105
OP_AUTH_ROLE_DELETE = 106
OP_AUTH_ROLE_GRANT = 107
OP_AUTH_ROLE_REVOKE = 108
OP_AUTH_ASSIGN_ROLE = 109
OP_AUTH_UNASSIGN_ROLE = 110
OP_AUTH_GRANT_KEY = 111
OP_AUTH_REVOKE_KEY_GRANT = 112
OP_AUTH_RATELIMIT_LIST = 113
OP_AUTH_RATELIMIT_CLEAR = 114
OP_AUTH_WHOAMI = 115

KEY_CONFIRM_TAG = 0xF1

STATUS_OK = 0
STATUS_AUTH_FAILED = 5
STATUS_PERMISSION_DENIED = 6
STATUS_RATE_LIMITED = 7

FIELD_TYPES = {
    "int32": 0,
    "int64": 1,
    "float64": 2,
    "bool": 3,
    "dict_int32": 4,
    "string": 5,
    "bytes": 6,
    "vector_float32": 9,
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
                 default_db: str = "default",
                 identity_key_path: str | None = None,
                 known_hosts_mode: str = "strict",
                 known_hosts_path: str | None = None) -> None:
        self._host = host
        self._port = port
        self._default_db = default_db
        self._sock: socket.socket | None = None
        self._next_id = 1
        self._identity_key_path = (
            Path(identity_key_path).expanduser() if identity_key_path else None
        )
        self._key_c2s: bytes | None = None
        self._key_s2c: bytes | None = None
        self._send_seq = 0
        self._recv_seq = 0
        self._aead_c2s: ChaCha20Poly1305 | None = None
        self._aead_s2c: ChaCha20Poly1305 | None = None
        self._session_id: bytes | None = None
        self._session_fingerprint: str | None = None
        self._known_hosts_path = (
            Path(known_hosts_path).expanduser()
            if known_hosts_path
            else Path.home() / ".mimicdb" / "known_hosts"
        )
        self._known_hosts_mode = known_hosts_mode
        allow_skip = os.environ.get("MIMICDB_ALLOW_KNOWN_HOSTS_SKIP", "")
        self._allow_known_hosts_skip = allow_skip.strip().lower() in {"1", "true", "yes"}

    def connect(self) -> None:
        if self._sock is not None:
            return
        self._sock = socket.create_connection((self._host, self._port))
        self._handshake()

    def close(self) -> None:
        if self._sock is None:
            return
        self._sock.close()
        self._sock = None
        self._key_c2s = None
        self._key_s2c = None
        self._aead_c2s = None
        self._aead_s2c = None
        self._session_id = None
        self._session_fingerprint = None

    def set_default_db(self, name: str) -> None:
        self._default_db = name

    def ping(self) -> None:
        self._request(OP_PING, b"")

    def host_key(self) -> dict[str, str]:
        response = self._request(OP_HOST_KEY, b"")
        if len(response) < 2:
            raise ProtocolError("short host_key response")
        cursor = 0
        key_len = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        if cursor + key_len + 2 > len(response):
            raise ProtocolError("short host_key response")
        key = response[cursor:cursor + key_len]
        cursor += key_len
        fp_len = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        if cursor + fp_len > len(response):
            raise ProtocolError("short host_key response")
        fingerprint = response[cursor:cursor + fp_len].decode("utf-8")
        return {"key_hex": key.hex(), "fingerprint": fingerprint}

    def host_key_rotate(self) -> dict[str, str]:
        response = self._request(OP_HOST_KEY_ROTATE, b"")
        if len(response) < 2:
            raise ProtocolError("short host_key_rotate response")
        cursor = 0
        key_len = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        if cursor + key_len + 2 > len(response):
            raise ProtocolError("short host_key_rotate response")
        key = response[cursor:cursor + key_len]
        cursor += key_len
        fp_len = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        if cursor + fp_len > len(response):
            raise ProtocolError("short host_key_rotate response")
        fingerprint = response[cursor:cursor + fp_len].decode("utf-8")
        return {"key_hex": key.hex(), "fingerprint": fingerprint}

    def auth_init_root(self, public_key: bytes, comment: str = "") -> str:
        fingerprint = hashlib.sha256(public_key).hexdigest()
        comment_bytes = comment.encode("utf-8")
        payload = struct.pack("<H", len(public_key)) + public_key
        payload += struct.pack("<H", len(comment_bytes)) + comment_bytes
        payload += bytes([KEY_CONFIRM_TAG])
        payload += struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        response = self._request(OP_AUTH_INIT_ROOT, payload)
        if len(response) < 2:
            raise ProtocolError("short auth_init_root response")
        fp_len = struct.unpack_from("<H", response, 0)[0]
        if 2 + fp_len > len(response):
            raise ProtocolError("short auth_init_root response")
        return response[2:2 + fp_len].decode("utf-8")

    def auth_key_add(self, public_key: bytes, comment: str = "") -> str:
        fingerprint = hashlib.sha256(public_key).hexdigest()
        comment_bytes = comment.encode("utf-8")
        payload = struct.pack("<H", len(public_key)) + public_key
        payload += struct.pack("<H", len(comment_bytes)) + comment_bytes
        payload += bytes([KEY_CONFIRM_TAG])
        payload += struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        response = self._request(OP_AUTH_KEY_ADD, payload)
        if len(response) < 2:
            raise ProtocolError("short auth_key_add response")
        fp_len = struct.unpack_from("<H", response, 0)[0]
        if 2 + fp_len > len(response):
            raise ProtocolError("short auth_key_add response")
        return response[2:2 + fp_len].decode("utf-8")

    def auth_key_disable(self, fingerprint: str) -> None:
        payload = struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        self._request(OP_AUTH_KEY_DISABLE, payload)

    def auth_key_remove(self, fingerprint: str) -> None:
        payload = struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        self._request(OP_AUTH_KEY_REMOVE, payload)

    def auth_key_list(self) -> list[dict[str, str | bool]]:
        response = self._request(OP_AUTH_KEY_LIST, b"")
        if len(response) < 2:
            raise ProtocolError("short auth_key_list response")
        count = struct.unpack_from("<H", response, 0)[0]
        cursor = 2
        results = []
        for _ in range(count):
            if cursor + 2 > len(response):
                raise ProtocolError("short auth_key_list response")
            fp_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + fp_len + 1 + 2 > len(response):
                raise ProtocolError("short auth_key_list response")
            fingerprint = response[cursor:cursor + fp_len].decode("utf-8")
            cursor += fp_len
            enabled = response[cursor] != 0
            cursor += 1
            comment_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + comment_len > len(response):
                raise ProtocolError("short auth_key_list response")
            comment = response[cursor:cursor + comment_len].decode("utf-8")
            cursor += comment_len
            results.append({"fingerprint": fingerprint, "enabled": enabled, "comment": comment})
        return results

    def auth_role_create(self, role: str) -> None:
        payload = struct.pack("<H", len(role)) + role.encode("utf-8")
        self._request(OP_AUTH_ROLE_CREATE, payload)

    def auth_role_delete(self, role: str) -> None:
        payload = struct.pack("<H", len(role)) + role.encode("utf-8")
        self._request(OP_AUTH_ROLE_DELETE, payload)

    def auth_role_grant(self, role: str, cap: str, scope: str, revoke: bool = False) -> None:
        opcode = OP_AUTH_ROLE_REVOKE if revoke else OP_AUTH_ROLE_GRANT
        payload = (
            struct.pack("<H", len(role)) + role.encode("utf-8") +
            struct.pack("<H", len(cap)) + cap.encode("utf-8") +
            struct.pack("<H", len(scope)) + scope.encode("utf-8")
        )
        self._request(opcode, payload)

    def auth_assign_role(self, fingerprint: str, role: str, scope: str,
                         revoke: bool = False) -> None:
        opcode = OP_AUTH_UNASSIGN_ROLE if revoke else OP_AUTH_ASSIGN_ROLE
        payload = (
            struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8") +
            struct.pack("<H", len(role)) + role.encode("utf-8") +
            struct.pack("<H", len(scope)) + scope.encode("utf-8")
        )
        self._request(opcode, payload)

    def auth_grant_key(self, fingerprint: str, cap: str, scope: str,
                       revoke: bool = False) -> None:
        opcode = OP_AUTH_REVOKE_KEY_GRANT if revoke else OP_AUTH_GRANT_KEY
        payload = (
            struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8") +
            struct.pack("<H", len(cap)) + cap.encode("utf-8") +
            struct.pack("<H", len(scope)) + scope.encode("utf-8")
        )
        self._request(opcode, payload)

    def auth_ratelimit_list(self) -> list[dict[str, int | str]]:
        response = self._request(OP_AUTH_RATELIMIT_LIST, b"")
        if len(response) < 2:
            raise ProtocolError("short auth_ratelimit_list response")
        count = struct.unpack_from("<H", response, 0)[0]
        cursor = 2
        results = []
        for _ in range(count):
            if cursor + 2 > len(response):
                raise ProtocolError("short auth_ratelimit_list response")
            remote_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + remote_len + 2 > len(response):
                raise ProtocolError("short auth_ratelimit_list response")
            remote = response[cursor:cursor + remote_len].decode("utf-8")
            cursor += remote_len
            fp_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + fp_len + 4 + 8 + 8 > len(response):
                raise ProtocolError("short auth_ratelimit_list response")
            fingerprint = response[cursor:cursor + fp_len].decode("utf-8")
            cursor += fp_len
            fail_count = struct.unpack_from("<I", response, cursor)[0]
            cursor += 4
            next_allowed = struct.unpack_from("<q", response, cursor)[0]
            cursor += 8
            last_fail = struct.unpack_from("<q", response, cursor)[0]
            cursor += 8
            results.append({
                "remote": remote,
                "fingerprint": fingerprint,
                "fail_count": fail_count,
                "next_allowed_at": next_allowed,
                "last_fail_at": last_fail,
            })
        return results

    def auth_ratelimit_clear(self, remote: str, fingerprint: str = "") -> None:
        payload = struct.pack("<H", len(remote)) + remote.encode("utf-8")
        if fingerprint:
            payload += struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        self._request(OP_AUTH_RATELIMIT_CLEAR, payload)

    def auth_whoami(self) -> dict[str, object]:
        response = self._request(OP_AUTH_WHOAMI, b"")
        if len(response) < 2:
            raise ProtocolError("short auth_whoami response")
        cursor = 0
        fp_len = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        if cursor + fp_len + 2 > len(response):
            raise ProtocolError("short auth_whoami response")
        fingerprint = response[cursor:cursor + fp_len].decode("utf-8")
        cursor += fp_len
        role_count = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        roles = []
        for _ in range(role_count):
            if cursor + 2 > len(response):
                raise ProtocolError("short auth_whoami response")
            name_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + name_len + 2 > len(response):
                raise ProtocolError("short auth_whoami response")
            name = response[cursor:cursor + name_len].decode("utf-8")
            cursor += name_len
            scope_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + scope_len > len(response):
                raise ProtocolError("short auth_whoami response")
            scope = response[cursor:cursor + scope_len].decode("utf-8")
            cursor += scope_len
            roles.append({"role": name, "scope": scope})
        if cursor + 2 > len(response):
            raise ProtocolError("short auth_whoami response")
        grant_count = struct.unpack_from("<H", response, cursor)[0]
        cursor += 2
        grants = []
        for _ in range(grant_count):
            if cursor + 2 > len(response):
                raise ProtocolError("short auth_whoami response")
            cap_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + cap_len + 2 > len(response):
                raise ProtocolError("short auth_whoami response")
            cap = response[cursor:cursor + cap_len].decode("utf-8")
            cursor += cap_len
            scope_len = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            if cursor + scope_len > len(response):
                raise ProtocolError("short auth_whoami response")
            scope = response[cursor:cursor + scope_len].decode("utf-8")
            cursor += scope_len
            grants.append({"capability": cap, "scope": scope})
        if cursor != len(response):
            raise ProtocolError("unexpected auth_whoami payload")
        return {"fingerprint": fingerprint, "roles": roles, "grants": grants}

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
            if field_type in ("string", "bytes", "vector_float32"):
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

    def vector_search(
        self, dataset: str, field_index: int, query: Iterable[float], top_k: int = 10,
        metric: str = "cosine", database: str | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
    ) -> list[dict[str, int | float]]:
        metrics = {"cosine": 0, "dot": 1, "l2": 2, "l2_squared": 2}
        if metric not in metrics:
            raise ValueError("metric must be cosine, dot, or l2")
        values = [float(value) for value in query]
        if not values or top_k <= 0:
            raise ValueError("query must be non-empty and top_k must be positive")
        db_name = self._default_db if database is None else database
        payload = bytearray()
        payload += struct.pack("<H", len(db_name)) + db_name.encode("utf-8")
        payload += struct.pack("<H", len(dataset)) + dataset.encode("utf-8")
        payload += struct.pack("<HBII", field_index, metrics[metric], len(values), top_k)
        pred_list = predicates or []
        payload += struct.pack("<H", len(pred_list))
        for pred_field, pred_op, pred_value in pred_list:
            payload += struct.pack("<HBd", pred_field, pred_op, float(pred_value))
        payload += struct.pack(f"<{len(values)}f", *values)
        response = self._request(OP_VECTOR_SEARCH, bytes(payload))
        if len(response) < 4:
            raise ProtocolError("short vector search response")
        count = struct.unpack_from("<I", response, 0)[0]
        if len(response) != 4 + count * 12:
            raise ProtocolError("invalid vector search response")
        return [
            {"row_id": struct.unpack_from("<Q", response, 4 + i * 12)[0],
             "distance": struct.unpack_from("<f", response, 12 + i * 12)[0]}
            for i in range(count)
        ]

    def scan(
        self,
        dataset: str,
        fields: list[tuple[str, str]] | None = None,
        columns: list[str] | None = None,
        database: str | None = None,
        predicates: list[tuple[int, int, float]] | None = None,
        limit: int = 0,
        offset: int = 0,
    ) -> list[dict]:
        db_name = self._default_db if database is None else database
        field_index: dict[str, int] = {}
        column_indices: list[int] = []
        if fields is not None:
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
        response_field_order: list[int] = []
        for _ in range(field_count):
            if cursor + 8 > len(response):
                raise ProtocolError("short scan response")
            field_idx = struct.unpack_from("<H", response, cursor)[0]
            cursor += 2
            response_field_order.append(field_idx)
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

        if fields is None:
            columns = [f"col_{idx}" for idx in response_field_order]
            field_index = {name: idx for name, idx in zip(columns, response_field_order)}

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
        for attempt in range(2):
            if self._sock is None:
                try:
                    self.connect()
                except OSError as exc:
                    raise ProtocolError("connection failed") from exc
            request_id = self._next_id
            self._next_id += 1
            flags = 0
            request_payload = payload
            if self._session_id:
                flags |= FLAG_SESSION_ID
                request_payload = self._session_id + request_payload
            header = struct.pack(
                "<IHHHHII",
                MAGIC,
                VERSION,
                flags,
                opcode,
                0,
                len(request_payload),
                request_id,
            )
            assert self._sock is not None
            self._send_secure(header + request_payload)
            response = self._recv_secure()
            if len(response) < 20:
                raise ProtocolError("short response header")
            resp_header = response[:20]
            magic, version, flags, resp_opcode, status, size, resp_id = struct.unpack(
                "<IHHHHII", resp_header
            )
            if magic != MAGIC or version != VERSION:
                raise ProtocolError("invalid response header")
            if resp_opcode != opcode or resp_id != request_id:
                raise ProtocolError("mismatched response")
            response_payload = response[20:]
            if len(response_payload) != size:
                raise ProtocolError("mismatched response payload size")
            if status == STATUS_OK:
                return response_payload
            if status == STATUS_AUTH_FAILED and attempt == 0 and self._session_id:
                self.close()
                continue
            raise ProtocolError(f"server error status={status}")
        raise ProtocolError("authentication failed")

    def _load_identity_key(self) -> ed25519.Ed25519PrivateKey:
        key_path = self._identity_key_path or (Path.home() / ".mimicdb" / "client_ed25519")
        if key_path.exists():
            data = key_path.read_bytes()
            return ed25519.Ed25519PrivateKey.from_private_bytes(data)
        key_path.parent.mkdir(parents=True, exist_ok=True)
        key = ed25519.Ed25519PrivateKey.generate()
        key_path.write_bytes(
            key.private_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PrivateFormat.Raw,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
        return key

    def _handshake(self) -> None:
        assert self._sock is not None
        client_nonce = os.urandom(32)
        client_eph = x25519.X25519PrivateKey.generate()
        client_eph_pub = client_eph.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
        identity_key = self._load_identity_key()
        identity_pub = identity_key.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )
        identity_fingerprint = hashlib.sha256(identity_pub).hexdigest()
        hello = struct.pack("<IHH", SEC_MAGIC, SEC_VERSION, CIPHER_CHACHA20_POLY1305)
        hello += client_nonce + client_eph_pub + identity_pub
        _send_all(self._sock, hello)

        server_hello = _recv_exact(self._sock, 136)
        magic, version, cipher = struct.unpack_from("<IHH", server_hello, 0)
        if magic != SEC_MAGIC or version != SEC_VERSION or cipher != CIPHER_CHACHA20_POLY1305:
            raise ProtocolError("invalid server hello")
        offset = 8
        server_nonce = server_hello[offset:offset + 32]
        offset += 32
        server_eph_pub = server_hello[offset:offset + 32]
        offset += 32
        host_key = server_hello[offset:offset + 32]
        offset += 32
        host_fingerprint = server_hello[offset:offset + 32].hex()
        self._verify_known_host(host_fingerprint, host_key.hex())

        shared = client_eph.exchange(
            x25519.X25519PublicKey.from_public_bytes(server_eph_pub)
        )
        hkdf = HKDF(
            algorithm=hashes.SHA256(),
            length=80,
            salt=client_nonce + server_nonce,
            info=b"mimicdb-session",
        )
        okm = hkdf.derive(shared)
        self._key_c2s = okm[:32]
        self._key_s2c = okm[32:64]
        self._session_id = okm[64:80]
        self._aead_c2s = ChaCha20Poly1305(self._key_c2s)
        self._aead_s2c = ChaCha20Poly1305(self._key_s2c)

        transcript = hello + server_hello
        transcript_hash = hashlib.sha256(transcript).digest()
        signature = identity_key.sign(transcript_hash)
        self._send_seq = 0
        self._recv_seq = 0
        self._send_secure(signature)
        accept = self._recv_secure()
        if len(accept) < 1 + 4:
            raise ProtocolError("short accept response")
        status = accept[0]
        wait_seconds = struct.unpack_from("<I", accept, 1)[0]
        if status == 2:
            raise ProtocolError(f"rate limited: wait {wait_seconds}s")
        if status != 1:
            raise ProtocolError("authentication failed")
        if len(accept) < 1 + 4 + 4 + 4 + 16 + 2 + 64:
            raise ProtocolError("short accept response")
        fp_offset = 1 + 4 + 4 + 4 + 16
        fp_len = struct.unpack_from("<H", accept, fp_offset)[0]
        fp_offset += 2
        sig_offset = fp_offset + fp_len
        if sig_offset + 64 > len(accept):
            raise ProtocolError("short accept response")
        fingerprint = accept[fp_offset:sig_offset].decode("utf-8")
        server_sig = accept[sig_offset:sig_offset + 64]
        server_transcript = hello + server_hello + signature
        server_hash = hashlib.sha256(server_transcript).digest()
        try:
            ed25519.Ed25519PublicKey.from_public_bytes(host_key).verify(
                server_sig, server_hash
            )
        except Exception as exc:
            raise ProtocolError("server signature invalid") from exc
        if fingerprint != identity_fingerprint:
            raise ProtocolError("client fingerprint mismatch")
        self._session_fingerprint = fingerprint
        self._send_seq = 1
        self._recv_seq = 1
        ack = struct.pack("<H", len(fingerprint)) + fingerprint.encode("utf-8")
        self._send_secure(ack)

    def _load_known_hosts(self) -> dict[str, tuple[str, str]]:
        entries: dict[str, tuple[str, str]] = {}
        if not self._known_hosts_path.exists():
            return entries
        for line in self._known_hosts_path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.strip().startswith("#"):
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            hostport, fingerprint, key_hex = parts[:3]
            entries[hostport] = (fingerprint, key_hex)
        return entries

    def _write_known_hosts(self, entries: dict[str, tuple[str, str]]) -> None:
        self._known_hosts_path.parent.mkdir(parents=True, exist_ok=True)
        lines = []
        for hostport, (fingerprint, key_hex) in sorted(entries.items()):
            lines.append(f"{hostport} {fingerprint} {key_hex}")
        self._known_hosts_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    def _verify_known_host(self, fingerprint: str, key_hex: str) -> None:
        if self._known_hosts_mode == "skip":
            if self._host in {"127.0.0.1", "localhost", "::1"}:
                return
            if self._allow_known_hosts_skip:
                return
            raise ProtocolError(
                "known_hosts_mode=skip requires localhost or MIMICDB_ALLOW_KNOWN_HOSTS_SKIP=1"
            )
        hostport = f"{self._host}:{self._port}"
        entries = self._load_known_hosts()
        if hostport in entries:
            known_fp, known_key = entries[hostport]
            if known_fp != fingerprint or known_key != key_hex:
                raise ProtocolError(
                    f"host key mismatch for {hostport}: expected {known_fp}/{known_key}, "
                    f"got {fingerprint}/{key_hex}"
                )
            return
        entries[hostport] = (fingerprint, key_hex)
        self._write_known_hosts(entries)

    def _send_secure(self, plaintext: bytes) -> None:
        if self._sock is None or self._aead_c2s is None:
            raise ProtocolError("secure channel not established")
        nonce = b"\x00" * 4 + struct.pack("<Q", self._send_seq)
        ciphertext = self._aead_c2s.encrypt(nonce, plaintext, None)
        header = struct.pack("<QI", self._send_seq, len(ciphertext))
        _send_all(self._sock, header + ciphertext)
        self._send_seq += 1

    def _recv_secure(self) -> bytes:
        if self._sock is None or self._aead_s2c is None:
            raise ProtocolError("secure channel not established")
        header = _recv_exact(self._sock, 12)
        seq, length = struct.unpack("<QI", header)
        if seq != self._recv_seq:
            raise ProtocolError("unexpected sequence number")
        ciphertext = _recv_exact(self._sock, length)
        nonce = b"\x00" * 4 + struct.pack("<Q", seq)
        plaintext = self._aead_s2c.decrypt(nonce, ciphertext, None)
        self._recv_seq += 1
        return plaintext


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
        elif field_type == "vector_float32":
            try:
                vector = [float(item) for item in value]
            except (TypeError, ValueError) as exc:
                raise ValueError("vector_float32 field requires a numeric sequence") from exc
            encoded = struct.pack(f"<{len(vector)}f", *vector)
            lengths += struct.pack("<I", len(encoded))
            data += encoded
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
