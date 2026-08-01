import hashlib
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives.asymmetric import x25519
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

from client.mimicdb_client import MimicDBClient, ProtocolError


def _find_free_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    _, port = sock.getsockname()
    sock.close()
    return port


def _write_config(path: Path, storage_root: Path, port: int,
                  bind_host: str = "127.0.0.1",
                  extra_lines: list[str] | None = None) -> None:
    lines = [
        f"bind={bind_host}:{port}",
        f"storage_root={storage_root}",
        "flush_on_shutdown=true",
    ]
    if extra_lines:
        lines.extend(extra_lines)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _start_server(server_bin: str, config_path: Path) -> subprocess.Popen:
    proc = subprocess.Popen([server_bin, "--config", str(config_path)])
    time.sleep(0.3)
    return proc


def _stop_server(proc: subprocess.Popen) -> None:
    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()


def _make_keypair(path: Path) -> bytes:
    key = ed25519.Ed25519PrivateKey.generate()
    priv_bytes = key.private_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PrivateFormat.Raw,
        encryption_algorithm=serialization.NoEncryption(),
    )
    path.write_bytes(priv_bytes)
    return key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )


def _scan_auth_rows(client: MimicDBClient, dataset: str,
                    fields: list[tuple[str, str]]) -> int:
    rows = client.scan(dataset, fields, database="__auth__")
    return len(rows)


def _start_fake_server_missing_signature(port: int) -> tuple[threading.Thread, threading.Event]:
    ready = threading.Event()

    def _server() -> None:
        def _read_exact(sock: socket.socket, size: int) -> bytes:
            chunks = []
            remaining = size
            while remaining > 0:
                data = sock.recv(remaining)
                if not data:
                    break
                chunks.append(data)
                remaining -= len(data)
            return b"".join(chunks)

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", port))
        sock.listen(1)
        ready.set()
        conn, _ = sock.accept()
        try:
            client_hello = _read_exact(conn, 104)
            if len(client_hello) < 104:
                return
            magic, version, cipher = struct.unpack_from("<IHH", client_hello, 0)
            if magic != 0x4D534543 or version != 1 or cipher != 1:
                return
            offset = 8
            client_nonce = client_hello[offset:offset + 32]
            offset += 32
            client_eph_pub = client_hello[offset:offset + 32]
            offset += 32
            server_nonce = os.urandom(32)
            server_eph = x25519.X25519PrivateKey.generate()
            server_eph_pub = server_eph.public_key().public_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PublicFormat.Raw,
            )
            host_key = ed25519.Ed25519PrivateKey.generate().public_key().public_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PublicFormat.Raw,
            )
            host_fp = hashlib.sha256(host_key).digest()
            server_hello = struct.pack("<IHH", magic, version, cipher)
            server_hello += server_nonce + server_eph_pub + host_key + host_fp
            conn.sendall(server_hello)
            shared = server_eph.exchange(
                x25519.X25519PublicKey.from_public_bytes(client_eph_pub)
            )
            hkdf = HKDF(
                algorithm=hashes.SHA256(),
                length=80,
                salt=client_nonce + server_nonce,
                info=b"mimicdb-session",
            )
            okm = hkdf.derive(shared)
            key_c2s = okm[:32]
            key_s2c = okm[32:64]

            header = _read_exact(conn, 12)
            if len(header) < 12:
                return
            seq = struct.unpack_from("<Q", header, 0)[0]
            length = struct.unpack_from("<I", header, 8)[0]
            ciphertext = _read_exact(conn, length)
            if len(ciphertext) < length:
                return
            aead_c2s = ChaCha20Poly1305(key_c2s)
            nonce = b"\x00" * 4 + struct.pack("<Q", seq)
            aead_c2s.decrypt(nonce, ciphertext, None)

            payload = b"\x01" + struct.pack("<I", 0)
            aead_s2c = ChaCha20Poly1305(key_s2c)
            nonce = b"\x00" * 4 + struct.pack("<Q", 0)
            out = aead_s2c.encrypt(nonce, payload, None)
            conn.sendall(struct.pack("<QI", 0, len(out)) + out)
        finally:
            conn.close()
            sock.close()

    thread = threading.Thread(target=_server, daemon=True)
    thread.start()
    ready.wait(timeout=2)
    return thread, ready


class TestSecurityV1(unittest.TestCase):
    def setUp(self) -> None:
        self.server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not self.server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")
        self._orig_home = os.environ.get("HOME", "")
        self._home_dir = tempfile.TemporaryDirectory()
        os.environ["HOME"] = self._home_dir.name
        os.environ["PYTHONPATH"] = os.environ.get("PYTHONPATH", "")

    def tearDown(self) -> None:
        os.environ["HOME"] = self._orig_home
        self._home_dir.cleanup()

    def _init_root(self, port: int, storage_root: Path) -> tuple[Path, bytes, str]:
        root_key_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / "root"
        root_key_path.parent.mkdir(parents=True, exist_ok=True)
        root_pub = _make_keypair(root_key_path)
        client = MimicDBClient(
            host="127.0.0.1",
            port=port,
            identity_key_path=str(root_key_path),
        )
        fp = client.auth_init_root(root_pub, comment="root")
        return root_key_path, root_pub, fp

    def test_handshake_and_auth_db_visibility(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, root_pub, fp = self._init_root(port, storage_root)
                expected_fp = hashlib.sha256(root_pub).hexdigest()
                self.assertEqual(fp, expected_fp)
                client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                client.ping()
                dbs = client.list_databases()
                self.assertNotIn("__auth__", dbs)
            finally:
                _stop_server(proc)

    def test_invalid_key_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                self._init_root(port, storage_root)
                bad_key_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / "bad"
                bad_key_path.parent.mkdir(parents=True, exist_ok=True)
                _make_keypair(bad_key_path)
                client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(bad_key_path),
                )
                with self.assertRaises(ProtocolError):
                    client.ping()
            finally:
                _stop_server(proc)

    def test_rate_limit_backoff(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, root_pub, _ = self._init_root(port, storage_root)
                bad_key_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / "bad"
                bad_key_path.parent.mkdir(parents=True, exist_ok=True)
                bad_pub = _make_keypair(bad_key_path)
                bad_fp = hashlib.sha256(bad_pub).hexdigest()
                for _ in range(4):
                    client = MimicDBClient(
                        host="127.0.0.1",
                        port=port,
                        identity_key_path=str(bad_key_path),
                    )
                    with self.assertRaises(ProtocolError):
                        client.ping()
                root_client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                entries = root_client.auth_ratelimit_list()
                matches = [
                    entry
                    for entry in entries
                    if entry["remote"] == "127.0.0.1"
                    and entry["fingerprint"] == bad_fp
                ]
                self.assertTrue(matches)
                self.assertGreaterEqual(matches[0]["fail_count"], 4)
                self.assertGreaterEqual(matches[0]["next_allowed_at"], matches[0]["last_fail_at"])
            finally:
                _stop_server(proc)

    def test_multi_client_session_isolation(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, _, _ = self._init_root(port, storage_root)
                client_a = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                client_a.ping()
                session_a = client_a._session_id
                client_a.close()

                client_b = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                client_b.ping()
                session_b = client_b._session_id
                client_b.close()

                self.assertIsNotNone(session_a)
                self.assertIsNotNone(session_b)
                self.assertNotEqual(session_a, session_b)
            finally:
                _stop_server(proc)

    def test_host_key_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, _, _ = self._init_root(port, storage_root)
                known_hosts = Path(self._home_dir.name) / ".mimicdb" / "known_hosts"
                known_hosts.parent.mkdir(parents=True, exist_ok=True)
                bad_fp = "00" * 32
                bad_key = "11" * 32
                known_hosts.write_text(
                    f"127.0.0.1:{port} {bad_fp} {bad_key}\n",
                    encoding="utf-8",
                )
                client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                with self.assertRaises(ProtocolError):
                    client.ping()
            finally:
                _stop_server(proc)

    def test_known_hosts_skip_restricted(self) -> None:
        client = MimicDBClient(host="10.0.0.1", port=9000, known_hosts_mode="skip")
        with self.assertRaises(ProtocolError):
            client._verify_known_host("00" * 32, "11" * 32)
        original = os.environ.get("MIMICDB_ALLOW_KNOWN_HOSTS_SKIP")
        os.environ["MIMICDB_ALLOW_KNOWN_HOSTS_SKIP"] = "1"
        try:
            client = MimicDBClient(host="10.0.0.1", port=9000, known_hosts_mode="skip")
            client._verify_known_host("00" * 32, "11" * 32)
        finally:
            if original is None:
                os.environ.pop("MIMICDB_ALLOW_KNOWN_HOSTS_SKIP", None)
            else:
                os.environ["MIMICDB_ALLOW_KNOWN_HOSTS_SKIP"] = original

    def test_missing_server_signature_rejected(self) -> None:
        port = _find_free_port()
        server_thread, _ = _start_fake_server_missing_signature(port)
        identity_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / "client"
        identity_path.parent.mkdir(parents=True, exist_ok=True)
        _make_keypair(identity_path)
        client = MimicDBClient(
            host="127.0.0.1",
            port=port,
            identity_key_path=str(identity_path),
        )
        with self.assertRaises(ProtocolError):
            client.ping()
        server_thread.join(timeout=1)

    def test_rate_limit_and_audit_retention(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(
                config_path,
                storage_root,
                port,
                extra_lines=[
                    "auth_audit_log_max_rows=5",
                    "auth_rate_limit_max_rows=5",
                    "auth_prune_batch_rows=0",
                ],
            )
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, _, _ = self._init_root(port, storage_root)
                bad_key_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / "bad"
                bad_key_path.parent.mkdir(parents=True, exist_ok=True)
                _make_keypair(bad_key_path)
                for _ in range(8):
                    client = MimicDBClient(
                        host="127.0.0.1",
                        port=port,
                        identity_key_path=str(bad_key_path),
                    )
                    with self.assertRaises(ProtocolError):
                        client.ping()
                time.sleep(0.5)
                root_client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                audit_fields = [
                    ("ts", "int64"),
                    ("event_type", "string"),
                    ("remote_addr", "string"),
                    ("fingerprint", "string"),
                    ("details_json", "string"),
                ]
                rate_fields = [
                    ("remote_addr", "string"),
                    ("fingerprint", "string"),
                    ("fail_count", "int32"),
                    ("next_allowed_at", "int64"),
                    ("last_fail_at", "int64"),
                ]
                audit_rows = _scan_auth_rows(root_client, "audit_log", audit_fields)
                rate_rows = _scan_auth_rows(root_client, "rate_limits", rate_fields)
                self.assertLessEqual(audit_rows, 5)
                self.assertLessEqual(rate_rows, 5)
            finally:
                _stop_server(proc)

    def test_session_idle_timeout_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(
                config_path,
                storage_root,
                port,
                extra_lines=[
                    "session_idle_timeout_sec=1",
                    "session_max_lifetime_sec=0",
                ],
            )
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, _, _ = self._init_root(port, storage_root)
                client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                client.ping()
                old_session = client._session_id
                time.sleep(1.2)
                client.ping()
                self.assertNotEqual(client._session_id, old_session)
            finally:
                _stop_server(proc)

    def test_session_lifetime_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(
                config_path,
                storage_root,
                port,
                extra_lines=[
                    "session_idle_timeout_sec=3600",
                    "session_max_lifetime_sec=1",
                ],
            )
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, _, _ = self._init_root(port, storage_root)
                client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                client.ping()
                old_session = client._session_id
                time.sleep(1.2)
                client.ping()
                self.assertNotEqual(client._session_id, old_session)
            finally:
                _stop_server(proc)

    def test_authz_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port)
            proc = _start_server(self.server_bin, config_path)
            try:
                root_key_path, root_pub, _ = self._init_root(port, storage_root)
                root_client = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(root_key_path),
                )
                root_client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                )
                root_client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {"age": [1], "income": [10.0]},
                )
                root_client.create_dataset(
                    "orders",
                    [
                        ("total", "float64"),
                    ],
                )

                def add_key(name: str) -> tuple[Path, str]:
                    key_path = Path(self._home_dir.name) / ".mimicdb" / "keys" / name
                    key_path.parent.mkdir(parents=True, exist_ok=True)
                    pub = _make_keypair(key_path)
                    fp = root_client.auth_key_add(pub, comment=name)
                    return key_path, fp

                reader_key, reader_fp = add_key("reader")
                writer_key, writer_fp = add_key("writer")
                admin_key, admin_fp = add_key("admin")
                restricted_key, restricted_fp = add_key("restricted")

                root_client.auth_assign_role(reader_fp, "reader", "*")
                root_client.auth_assign_role(writer_fp, "writer", "*")
                root_client.auth_assign_role(admin_fp, "admin", "*")
                root_client.auth_role_create("ds_reader")
                root_client.auth_role_grant("ds_reader", "dataset.read", "dataset:default.users")
                root_client.auth_role_grant("ds_reader", "query.scan", "dataset:default.users")
                root_client.auth_assign_role(
                    restricted_fp, "ds_reader", "dataset:default.users"
                )

                reader = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(reader_key),
                )
                reader.scan("users", limit=1)
                with self.assertRaises(ProtocolError):
                    reader.append_batch(
                        "users",
                        [
                            ("age", "int32"),
                            ("income", "float64"),
                        ],
                        {"age": [2], "income": [20.0]},
                    )

                writer = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(writer_key),
                )
                writer.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {"age": [3], "income": [30.0]},
                )
                with self.assertRaises(ProtocolError):
                    writer.drop_database("default")

                admin = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(admin_key),
                )
                admin.create_database("tempdb")
                admin.drop_database("tempdb")
                with self.assertRaises(ProtocolError):
                    admin.auth_key_add(root_pub, comment="nope")

                restricted = MimicDBClient(
                    host="127.0.0.1",
                    port=port,
                    identity_key_path=str(restricted_key),
                )
                restricted.scan("users", limit=1)
                with self.assertRaises(ProtocolError):
                    restricted.scan("orders", limit=1)
            finally:
                _stop_server(proc)

    def test_local_only_bootstrap(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            port = _find_free_port()
            config_path = Path(tmpdir) / "mimicdb.conf"
            storage_root = Path(tmpdir) / "data"
            _write_config(config_path, storage_root, port, bind_host="0.0.0.0")
            result = subprocess.run(
                [self.server_bin, "--config", str(config_path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=5,
            )
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
