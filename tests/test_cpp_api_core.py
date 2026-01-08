import unittest

from mimicapi import CppApiClient


class TestCppApiCore(unittest.TestCase):
    def setUp(self) -> None:
        self.client = CppApiClient()
        self.client.create_database("default")
        self.client.create_dataset(
            "default",
            "docs",
            {
                "age": "int32",
                "name": "string",
                "blob": "bytes",
            },
        )

    def test_string_bytes_predicates(self) -> None:
        self.client.append_batch(
            "default",
            "docs",
            {
                "age": [10, 20, 30],
                "name": ["alpha", "beta", None],
                "blob": [b"a", b"b", None],
            },
        )
        rows = self.client.scan(
            "default",
            "docs",
            predicates=[(1, "eq", "beta")],
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["name"], "beta")

        rows = self.client.scan(
            "default",
            "docs",
            predicates=[(2, "ne", b"a")],
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["blob"], b"b")

    def test_null_predicates(self) -> None:
        self.client.append_batch(
            "default",
            "docs",
            {
                "age": [1, 2],
                "name": [None, "ok"],
                "blob": [None, b"x"],
            },
        )
        rows = self.client.scan(
            "default",
            "docs",
            predicates=[(1, "is_null", 0.0)],
        )
        self.assertEqual(len(rows), 1)
        self.assertIsNone(rows[0]["name"])
        rows = self.client.scan(
            "default",
            "docs",
            predicates=[(2, "is_not_null", 0.0)],
        )
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["blob"], b"x")

    def test_type_mismatch_guard(self) -> None:
        self.client.append_batch(
            "default",
            "docs",
            {
                "age": [1],
                "name": ["alpha"],
                "blob": [b"x"],
            },
        )
        with self.assertRaises(TypeError):
            self.client.scan("default", "docs", predicates=[(1, "eq", 1.0)])

    def test_scan_projection_and_limits(self) -> None:
        client = CppApiClient()
        client.create_database("default")
        client.create_dataset(
            "default",
            "mix",
            {
                "i32": "int32",
                "i64": "int64",
                "f64": "float64",
                "flag": "bool",
                "text": "string",
                "blob": "bytes",
            },
        )
        client.append_batch(
            "default",
            "mix",
            {
                "i32": [1, 2, 3, 4],
                "i64": [10, 20, 30, 40],
                "f64": [1.5, 2.5, 3.5, 4.5],
                "flag": [True, False, True, False],
                "text": ["a", "b", None, "d"],
                "blob": [b"x", b"y", b"z", None],
            },
        )
        rows = client.scan("default", "mix", columns=["text", "blob"], limit=2, offset=1)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["text"], "b")
        self.assertEqual(rows[1]["text"], None)

    def test_aggregate_numeric_predicates(self) -> None:
        client = CppApiClient()
        client.create_database("default")
        client.create_dataset(
            "default",
            "agg",
            {
                "age": "int32",
                "income": "float64",
            },
        )
        client.append_batch(
            "default",
            "agg",
            {
                "age": [10, 20, 30, 40],
                "income": [100.0, 200.0, 300.0, 400.0],
            },
        )
        result = client.aggregate(
            "default",
            "agg",
            field_index=1,
            predicates=[(0, "gt", 20.0)],
        )
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["sum"], 700.0)

    def test_aggregate_rows_scanned(self) -> None:
        client = CppApiClient()
        client.create_database("default")
        client.create_dataset(
            "default",
            "scanrows",
            {
                "age": "int32",
                "income": "float64",
            },
        )
        client.append_batch(
            "default",
            "scanrows",
            {
                "age": [1, 2, 3],
                "income": [10.0, 20.0, 30.0],
            },
        )
        result = client.aggregate("default", "scanrows", field_index=1, predicates=[])
        self.assertEqual(result["rows_scanned"], 3)
        with self.assertRaises(TypeError):
            client.aggregate("default", "scanrows", field_index=0, predicates=[(1, "eq", "x")])

    def test_append_batch_varlen(self) -> None:
        client = CppApiClient()
        client.create_database("default")
        client.create_dataset(
            "default",
            "varlen",
            {
                "name": "string",
                "blob": "bytes",
            },
        )
        client.append_batch(
            "default",
            "varlen",
            {
                "name": ["alpha", None, "gamma"],
                "blob": [b"x", b"y", None],
            },
        )
        rows = client.scan("default", "varlen")
        self.assertEqual(rows[0]["name"], "alpha")
        self.assertEqual(rows[1]["name"], None)
        self.assertEqual(rows[1]["blob"], b"y")

    def test_error_paths(self) -> None:
        client = CppApiClient()
        with self.assertRaises(RuntimeError):
            client.scan("default", "missing")
        client.create_database("default")
        client.create_dataset(
            "default",
            "errors",
            {
                "age": "int32",
            },
        )
        client.append_batch("default", "errors", {"age": [1, 2]})
        with self.assertRaises(RuntimeError):
            client.scan("default", "errors", columns=["missing"])


if __name__ == "__main__":
    unittest.main()
