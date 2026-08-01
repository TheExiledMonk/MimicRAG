import unittest

from mimicapi import Dataset


class TestPythonAPI(unittest.TestCase):
    def test_vector_search_embedded(self):
        vectors = Dataset("vectors", {"embedding": "vector_float32", "tenant": "int32"})
        vectors.append_batch({
            "embedding": [[1.0, 0.0], [0.0, 1.0], [0.9, 0.1]],
            "tenant": [1, 2, 1],
        })
        hits = vectors.vector_search("embedding", [1.0, 0.0], top_k=2,
                                     predicates=[(1, 0, 1.0)])
        self.assertEqual([hit["row_id"] for hit in hits], [0, 2])
        with self.assertRaises((ValueError, RuntimeError)):
            vectors.vector_search("embedding", [1.0, 0.0, 0.0])

    def test_filter_and_aggregate(self):
        users = Dataset(
            name="users",
            fields={
                "age": "int32",
                "country": "int32",
                "income": "float64",
            },
        )

        users.append(age=34, country=45, income=82000.0)
        users.append(age=21, country=45, income=12000.0)
        users.append(age=40, country=33, income=50000.0)
        users.append(age=37, country=45, income=None)

        result = users.filter(age_gt=30, country_eq=45).aggregate(sum="income", count=True)
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["sum"], 82000.0)

    def test_min_max(self):
        users = Dataset(
            name="users",
            fields={
                "age": "int32",
            },
        )
        users.append(age=10)
        users.append(age=50)
        users.append(age=None)
        users.append(age=30)

        result = users.aggregate(min="age", max="age", count=True)
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["min"], 10)
        self.assertEqual(result["max"], 50)

    def test_append_batch(self):
        users = Dataset(
            name="users",
            fields={
                "age": "int32",
                "country": "int32",
                "income": "float64",
            },
        )
        users.append_batch(
            {
                "age": [10, 20, 30],
                "country": [1, 1, 2],
                "income": [100.0, None, 300.0],
            }
        )
        result = users.filter(country_eq=1).aggregate(count=True, sum="income")
        self.assertEqual(result["count"], 2)
        self.assertEqual(result["sum"], 100.0)

    def test_scan_projection(self):
        users = Dataset(
            name="users",
            fields={
                "age": "int32",
                "country": "int32",
            },
        )
        users.append(age=10, country=1)
        users.append(age=20, country=2)
        rows = users.scan(columns=["age"], country_eq=2)
        self.assertEqual(rows, [{"age": 20}])

    def test_string_field_scan(self):
        users = Dataset(
            name="users",
            fields={
                "name": "string",
            },
        )
        users.append(name="alice")
        users.append(name="bob")
        rows = users.scan(columns=["name"])
        self.assertEqual(rows[0]["name"], "alice")

    def test_pruning_debug_stats(self):
        users = Dataset(
            name="users",
            fields={
                "age": "int32",
            },
        )
        users.append(age=10)
        users.append(age=20)
        result, stats = users.query(age_gt=5, debug=True)
        self.assertIsInstance(result, dict)
        self.assertEqual(stats["segments_total"], 1)
        self.assertEqual(stats["segments_scanned"], 1)
        self.assertEqual(stats["segments_pruned"], 0)


if __name__ == "__main__":
    unittest.main()
