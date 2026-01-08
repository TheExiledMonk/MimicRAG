import unittest

from mimicapi.sql_parser import parse_sql


class TestSQLUnsupported(unittest.TestCase):
    def test_reject_join(self) -> None:
        with self.assertRaises(ValueError):
            parse_sql("SELECT a FROM foo JOIN bar ON foo.id = bar.id")

    def test_reject_subquery(self) -> None:
        with self.assertRaises(ValueError):
            parse_sql("SELECT a FROM foo WHERE id IN (SELECT id FROM bar)")

    def test_reject_window(self) -> None:
        with self.assertRaises(ValueError):
            parse_sql("SELECT SUM(x) OVER (PARTITION BY y) FROM foo")

    def test_reject_transaction(self) -> None:
        with self.assertRaises(ValueError):
            parse_sql("BEGIN TRANSACTION")


if __name__ == "__main__":
    unittest.main()
