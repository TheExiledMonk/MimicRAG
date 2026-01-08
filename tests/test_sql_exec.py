import unittest

from mimicapi.api_client import ApiClient
from mimicapi.sql_ast import CanonicalQuery, Filter, FilterExpr, Projection
from mimicapi.sql_exec import execute_query


class DummyClient(ApiClient):
    def __init__(self, rows: list[dict]) -> None:
        super().__init__()
        self._rows = rows

    def scan_routed(self, db: str, dataset: str, fields, columns=None,
                    predicates=None, limit=0, offset=0):
        _ = db
        _ = dataset
        _ = predicates
        _ = limit
        _ = offset
        return self._rows, {"rows_returned": len(self._rows)}


class TestSQLExec(unittest.TestCase):
    def test_exec_basic(self) -> None:
        rows = [{"name": "alpha", "age": 10}, {"name": "beta", "age": 20}]
        client = DummyClient(rows)
        fields = [("name", "string"), ("age", "int64")]
        query = CanonicalQuery(
            projections=[Projection(column="name")],
            dataset="users",
            filters=FilterExpr(filter=Filter(column="age", op="GT", value=10)),
            group_keys=[],
            having_filters=None,
            order_keys=[],
        )
        result, _ = execute_query(client, "default", query, fields)
        self.assertEqual(result, rows)


if __name__ == "__main__":
    unittest.main()
