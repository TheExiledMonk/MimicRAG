import unittest

from mimicapi.sql_parser import parse_sql


class TestSQLParser(unittest.TestCase):
    def test_dialect_normalization(self) -> None:
        query = "SELECT name FROM users WHERE age BETWEEN 10 AND 20"
        parsed = parse_sql(query, dialect="postgres")
        self.assertEqual(parsed.dataset, "users")
        self.assertTrue(parsed.filters)

        query = "SELECT name FROM users WHERE NVL(age, 0) >= 10"
        parsed = parse_sql(query, dialect="oracle")
        self.assertTrue(parsed.filters)

        query = "SELECT TOP 5 name FROM users"
        parsed = parse_sql(query, dialect="sqlserver")
        self.assertEqual(parsed.limit, 5)

        query = "SELECT name FROM users FETCH FIRST 3 ROWS ONLY"
        parsed = parse_sql(query, dialect="oracle")
        self.assertEqual(parsed.limit, 3)

        query = "SELECT name FROM users WHERE age IN (1,2,3)"
        parsed = parse_sql(query, dialect="mysql")
        self.assertTrue(parsed.filters)


if __name__ == "__main__":
    unittest.main()
