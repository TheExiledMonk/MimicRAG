import unittest

from mimicapi.sql_parser import parse_sql
from mimicapi.sql_ast import serialize_query


class TestSQLCanonical(unittest.TestCase):
    def test_ast_equivalence(self) -> None:
        query_a = "SELECT name FROM users WHERE age = 10"
        query_b = "SELECT name FROM users WHERE age = 10"
        parsed_a = serialize_query(parse_sql(query_a, dialect="ansi"))
        parsed_b = serialize_query(parse_sql(query_b, dialect="postgres"))
        self.assertEqual(parsed_a, parsed_b)


if __name__ == "__main__":
    unittest.main()
