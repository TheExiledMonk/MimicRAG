import unittest

from api.mimicapi.api_client import ApiClient
from api.mimicapi.transport import LocalTransport


class TestApiClient(unittest.TestCase):
    def test_single_server_fanout(self) -> None:
        client = ApiClient()
        transport = LocalTransport()
        client.add_server("local", local=True)
        client.register_transport("local", transport)

        fields = [("value", "int32")]
        client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [1, 2, 3]},
            quorum=1,
        )
        result = client.query_agg_routed("default", "numbers", 0)[0]
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["sum"], 6.0)

    def test_multiple_servers_quorum(self) -> None:
        client = ApiClient()
        transport_a = LocalTransport()
        transport_b = LocalTransport()
        client.add_server("a", local=True)
        client.add_server("b", local=True)
        client.register_transport("a", transport_a)
        client.register_transport("b", transport_b)

        fields = [("value", "int32")]
        client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [5, 5]},
            quorum=2,
        )
        result = client.fanout_query_agg("default", "numbers", 0)["result"]
        self.assertEqual(result["sum"], 20.0)

    def test_failure_injection(self) -> None:
        class FailingTransport(LocalTransport):
            def append_batch(self, *args, **kwargs):
                raise RuntimeError("fail")

        client = ApiClient()
        good = LocalTransport()
        bad = FailingTransport()
        client.add_server("good", local=True)
        client.add_server("bad", local=True)
        client.register_transport("good", good)
        client.register_transport("bad", bad)

        fields = [("value", "int32")]
        result = client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [1]},
            quorum=1,
        )
        self.assertTrue(result["stats"]["servers_failed"] >= 1)

    def test_idempotent_retry(self) -> None:
        client = ApiClient()
        transport = LocalTransport()
        client.add_server("local", local=True)
        client.register_transport("local", transport)
        fields = [("value", "int32")]
        result1 = client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [7]},
            quorum=1,
            batch_id=123,
        )
        result2 = client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [7]},
            quorum=1,
            batch_id=123,
        )
        self.assertTrue(result1["stats"]["servers_succeeded"] >= 1)
        self.assertTrue(result2["stats"]["servers_succeeded"] >= 1)
        agg = client.query_agg_routed("default", "numbers", 0)[0]
        self.assertEqual(agg["count"], 1)

    def test_quorum_read(self) -> None:
        client = ApiClient()
        transport_a = LocalTransport()
        transport_b = LocalTransport()
        client.add_server("a", local=True)
        client.add_server("b", local=True)
        client.register_transport("a", transport_a)
        client.register_transport("b", transport_b)

        fields = [("value", "int32")]
        client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [10]},
            quorum=2,
        )
        result, stats = client.query_agg_routed(
            "default",
            "numbers",
            0,
            consistency="quorum",
        )
        self.assertEqual(result["sum"], 20.0)
        self.assertEqual(stats["servers_succeeded"], 2)

    def test_partial_server_failure(self) -> None:
        class FlakyTransport(LocalTransport):
            def __init__(self) -> None:
                super().__init__()
                self.fail_next = True

            def append_batch(self, *args, **kwargs):
                if self.fail_next:
                    self.fail_next = False
                    raise RuntimeError("temporary failure")
                return super().append_batch(*args, **kwargs)

        client = ApiClient()
        good = LocalTransport()
        flaky = FlakyTransport()
        client.add_server("good", local=True)
        client.add_server("flaky", local=True)
        client.register_transport("good", good)
        client.register_transport("flaky", flaky)

        fields = [("value", "int32")]
        result = client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [1, 2]},
            quorum=1,
        )
        self.assertTrue(result["stats"]["servers_succeeded"] >= 1)

    def test_recovery_replay(self) -> None:
        client = ApiClient()
        good = LocalTransport()
        recovering = LocalTransport()
        client.add_server("good", local=True)
        client.add_server("recovering", local=True)
        client.register_transport("good", good)
        client.register_transport("recovering", recovering)

        fields = [("value", "int32")]
        client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [3]},
            quorum=1,
        )
        client.mark_failure("recovering")
        client.replay_for("recovering")
        result = client.fanout_query_agg("default", "numbers", 0)["result"]
        self.assertEqual(result["sum"], 6.0)

    def test_divergence_detection(self) -> None:
        client = ApiClient()
        a = LocalTransport()
        b = LocalTransport()
        client.add_server("a", local=True)
        client.add_server("b", local=True)
        client.register_transport("a", a)
        client.register_transport("b", b)
        fields = [("value", "int32")]
        client.append_batch_fanout(
            "default",
            "numbers",
            fields,
            {"value": [10]},
            quorum=1,
        )
        b.append_batch("default", "numbers", fields, {"value": [5]})
        result, stats = client.query_agg_routed(
            "default",
            "numbers",
            0,
            verify=True,
        )
        self.assertEqual(result["sum"], 10.0)
        self.assertGreaterEqual(stats["servers_contacted"], 2)


if __name__ == "__main__":
    unittest.main()
