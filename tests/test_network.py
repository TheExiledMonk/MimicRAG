import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization

from client.mimicdb_client import MimicDBClient, ProtocolError

STARTUP_DELAY = 0.3


class TestNetwork(unittest.TestCase):
    def _make_keypair(self, path: Path) -> bytes:
        key = ed25519.Ed25519PrivateKey.generate()
        priv_bytes = key.private_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PrivateFormat.Raw,
            encryption_algorithm=serialization.NoEncryption(),
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(priv_bytes)
        return key.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw,
        )

    def _init_root(self, port: int, home: Path, known_hosts: Path) -> Path:
        key_path = home / ".mimicdb" / "keys" / "root"
        pub = self._make_keypair(key_path)
        client = MimicDBClient(
            port=port,
            identity_key_path=str(key_path),
            known_hosts_path=str(known_hosts),
        )
        client.auth_init_root(pub, comment="root")
        client.close()
        return key_path
    def test_round_trip(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9011
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "mimicdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                root_key = self._init_root(port, home, known_hosts)
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.ping()
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                )
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {
                        "age": [30, 20, 40],
                        "income": [100.0, None, 300.0],
                    },
                )
                result = client.query_agg("users", field_index=1)
                self.assertEqual(result["count"], 2)
                self.assertEqual(result["sum"], 400.0)
                self.assertEqual(result["min"], 100.0)
                self.assertEqual(result["max"], 300.0)
                pred_result = client.query_agg(
                    "users",
                    field_index=1,
                    predicates=[(0, 4, 25.0)],
                )
                self.assertEqual(pred_result["count"], 2)
                self.assertEqual(pred_result["sum"], 400.0)
                client.create_dataset("vectors", [("embedding", "vector_float32")])
                client.append_batch(
                    "vectors", [("embedding", "vector_float32")],
                    {"embedding": [[1.0, 0.0], [0.0, 1.0], [0.9, 0.1]]},
                )
                vector_hits = client.vector_search("vectors", 0, [1.0, 0.0], top_k=2)
                self.assertEqual([hit["row_id"] for hit in vector_hits], [0, 2])
                vector_ann_hits = client.vector_search(
                    "vectors", 0, [1.0, 0.0], top_k=2,
                    approximate=True, probes=100000)
                self.assertEqual([hit["row_id"] for hit in vector_ann_hits], [0, 2])
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

    def test_namespace_isolation(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9013
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "mimicdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                root_key = self._init_root(port, home, known_hosts)
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.create_database("db1")
                client.create_database("db2")
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    database="db1",
                )
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {
                        "age": [1, 2],
                        "income": [10.0, 20.0],
                    },
                    database="db1",
                )
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    database="db2",
                )
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {
                        "age": [1, 2, 3],
                        "income": [1.0, 1.0, 1.0],
                    },
                    database="db2",
                )
                result_db1 = client.query_agg("users", field_index=1, database="db1")
                result_db2 = client.query_agg("users", field_index=1, database="db2")
                self.assertEqual(result_db1["sum"], 30.0)
                self.assertEqual(result_db2["sum"], 3.0)
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

    def test_restart_recovery(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9012
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "mimicdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                root_key = self._init_root(port, home, known_hosts)
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                )
                rows = 4096
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {
                        "age": list(range(rows)),
                        "income": [float(i) for i in range(rows)],
                    },
                )
                expected = client.query_agg("users", field_index=1)
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                recovered = client.query_agg("users", field_index=1)
                health = client.health()
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

        self.assertEqual(recovered["count"], expected["count"])
        self.assertEqual(recovered["sum"], expected["sum"])
        self.assertEqual(recovered["min"], expected["min"])
        self.assertEqual(recovered["max"], expected["max"])
        self.assertEqual(health["datasets"], 1)
        self.assertGreaterEqual(health["segments"], 1)
        self.assertEqual(health["rows"], rows)

    def test_active_segment_loss_on_restart(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9014
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "mimicdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=false\n"
                    "flush_on_seal=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                root_key = self._init_root(port, home, known_hosts)
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                )
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                        ("income", "float64"),
                    ],
                    {
                        "age": [1, 2, 3],
                        "income": [1.0, 2.0, 3.0],
                    },
                )
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                result = client.query_agg("users", field_index=1)
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

        self.assertEqual(result["count"], 0)
        self.assertEqual(result["sum"], 0.0)
        self.assertIsNone(result["min"])
        self.assertIsNone(result["max"])

    def test_discard_partial_segment(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9015
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "mimicdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=true\n"
                    "flush_on_seal=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                root_key = self._init_root(port, home, known_hosts)
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.create_dataset(
                    "users",
                    [
                        ("age", "int32"),
                    ],
                )
                client.append_batch(
                    "users",
                    [
                        ("age", "int32"),
                    ],
                    {
                        "age": [1] * 4096,
                    },
                )
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

            bad_path = os.path.join(data_root, "default", "users", "segment_999.mimicdb")
            with open(bad_path, "wb") as bad_file:
                bad_file.write(b"bad")

            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(STARTUP_DELAY)
                home = Path(tmpdir) / "home"
                known_hosts = home / ".mimicdb" / "known_hosts"
                client = MimicDBClient(
                    port=port,
                    identity_key_path=str(root_key),
                    known_hosts_path=str(known_hosts),
                )
                client.health()
                client.close()
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

            self.assertFalse(os.path.exists(bad_path))

    def test_invalid_port(self) -> None:
        client = MimicDBClient(port=6550)
        with self.assertRaises(OSError):
            client.connect()

    def test_protocol_error(self) -> None:
        client = MimicDBClient()
        with self.assertRaises(ProtocolError):
            client._request(999, b"")
