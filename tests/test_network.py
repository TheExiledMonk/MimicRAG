import os
import subprocess
import tempfile
import time
import unittest

from client.mimicdb_client import MimicDBClient, ProtocolError


class TestNetwork(unittest.TestCase):
    def test_round_trip(self) -> None:
        server_bin = os.environ.get("MIMICDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("MIMICDB_SERVER_BIN not set")

        port = 9011
        proc = subprocess.Popen([server_bin, str(port)])
        try:
            time.sleep(0.2)
            client = MimicDBClient(port=port)
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
            self.assertEqual(result["count"], 3)
            self.assertEqual(result["sum"], 400.0)
            self.assertEqual(result["min"], 100.0)
            self.assertEqual(result["max"], 300.0)
            pred_result = client.query_agg(
                "users",
                field_index=1,
                predicates=[(0, 4, 25.0)],
            )
            self.assertEqual(pred_result["count"], 1)
            self.assertEqual(pred_result["sum"], 300.0)
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
        proc = subprocess.Popen([server_bin, str(port)])
        try:
            time.sleep(0.2)
            client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
                time.sleep(0.2)
                client = MimicDBClient(port=port)
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
