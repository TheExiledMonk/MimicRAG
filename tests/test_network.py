import os
import subprocess
import tempfile
import time
import unittest

from client.pcdb_client import PCDBClient, ProtocolError


class TestNetwork(unittest.TestCase):
    def test_round_trip(self) -> None:
        server_bin = os.environ.get("PCDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("PCDB_SERVER_BIN not set")

        port = 9011
        proc = subprocess.Popen([server_bin, str(port)])
        try:
            time.sleep(0.2)
            client = PCDBClient(port=port)
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
            client.close()
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()

    def test_restart_recovery(self) -> None:
        server_bin = os.environ.get("PCDB_SERVER_BIN")
        if not server_bin:
            self.skipTest("PCDB_SERVER_BIN not set")

        port = 9012
        with tempfile.TemporaryDirectory() as tmpdir:
            data_root = os.path.join(tmpdir, "data")
            config_path = os.path.join(tmpdir, "pcdb.conf")
            with open(config_path, "w", encoding="utf-8") as config_file:
                config_file.write(
                    f"bind=127.0.0.1:{port}\n"
                    f"storage_root={data_root}\n"
                    "flush_on_shutdown=true\n"
                )
            proc = subprocess.Popen([server_bin, "--config", config_path])
            try:
                time.sleep(0.2)
                client = PCDBClient(port=port)
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
                client = PCDBClient(port=port)
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

    def test_invalid_port(self) -> None:
        client = PCDBClient(port=6550)
        with self.assertRaises(OSError):
            client.connect()

    def test_protocol_error(self) -> None:
        client = PCDBClient()
        with self.assertRaises(ProtocolError):
            client._request(999, b"")
