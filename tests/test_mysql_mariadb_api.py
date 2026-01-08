import unittest

from mimicapi.mysql_mariadb import MySQLConnection, SQLIntegrityError, SQLExecutionError


class TestMySQLMariaDBAPI(unittest.TestCase):
    def setUp(self) -> None:
        self.conn = MySQLConnection()
        self.cur = self.conn.cursor()
        self.cur.execute("CREATE DATABASE testdb")
        self.cur.execute("USE testdb")

    def test_ddl_dml_roundtrip(self) -> None:
        self.cur.execute("CREATE TABLE users (age INT, country INT, income DOUBLE)")
        self.cur.execute("INSERT INTO users (age, country, income) VALUES (34, 45, 82000)")
        self.cur.execute("INSERT INTO users (age, country, income) VALUES (21, 45, 12000)")
        self.cur.execute("UPDATE users SET income=90000 WHERE age=34")
        self.cur.execute("SELECT age, income FROM users WHERE country=45 ORDER BY age DESC")
        rows = self.cur.fetchall()
        self.assertEqual(rows[0][0], 34)
        self.cur.execute("DELETE FROM users WHERE age=21")
        self.cur.execute("SELECT age FROM users")
        remaining = self.cur.fetchall()
        self.assertEqual(len(remaining), 1)

    def test_select_filters(self) -> None:
        self.cur.execute("CREATE TABLE items (sku INT, price DOUBLE, tag VARCHAR(10))")
        self.cur.execute("INSERT INTO items (sku, price, tag) VALUES (1, 10.0, 'a')")
        self.cur.execute("INSERT INTO items (sku, price, tag) VALUES (2, 20.0, 'b')")
        self.cur.execute("INSERT INTO items (sku, price, tag) VALUES (3, 30.0, 'bb')")
        self.cur.execute("SELECT sku FROM items WHERE price >= 20 AND tag LIKE 'b%' ORDER BY sku")
        rows = self.cur.fetchall()
        self.assertEqual([row[0] for row in rows], [2, 3])

    def test_join_behavior(self) -> None:
        self.cur.execute("CREATE TABLE users (id INT, team INT)")
        self.cur.execute("CREATE TABLE teams (id INT, name VARCHAR(10))")
        self.cur.execute("INSERT INTO users (id, team) VALUES (1, 10)")
        self.cur.execute("INSERT INTO users (id, team) VALUES (2, 20)")
        self.cur.execute("INSERT INTO teams (id, name) VALUES (10, 'a')")
        self.cur.execute("SELECT users.id, teams.name FROM users INNER JOIN teams ON users.team = teams.id")
        rows = self.cur.fetchall()
        self.assertEqual(rows, [(1, "a")])

        self.cur.execute("SELECT users.id, teams.name FROM users LEFT JOIN teams ON users.team = teams.id ORDER BY users.id")
        rows = self.cur.fetchall()
        self.assertEqual(rows[0][0], 1)
        self.assertEqual(rows[1][0], 2)

    def test_type_mapping(self) -> None:
        self.cur.execute(
            "CREATE TABLE types (a INT, b BIGINT, c FLOAT, d DOUBLE, e BOOLEAN, f VARCHAR(10), g BLOB, h DATETIME)"
        )
        self.cur.execute(
            "INSERT INTO types (a, b, c, d, e, f, g, h) "
            "VALUES (1, 2, 3.5, 4.25, 1, 'hello', 0x0102, '2024-01-01 10:00:00')"
        )
        self.cur.execute("SELECT a, b, c, d, e, f, g, h FROM types")
        row = self.cur.fetchone()
        self.assertEqual(row[0], 1)
        self.assertEqual(row[4], 1)
        self.assertEqual(row[5], "hello")
        self.assertEqual(row[6], b"\x01\x02")
        self.assertEqual(row[7], "2024-01-01 10:00:00")

    def test_error_parity(self) -> None:
        self.cur.execute("CREATE TABLE dup (id INT)")
        self.cur.execute("INSERT INTO dup (id) VALUES (1)")
        with self.assertRaises(SQLIntegrityError):
            self.cur.execute("INSERT INTO dup (id) VALUES (1)")
        with self.assertRaises(SQLExecutionError):
            self.cur.execute("INSERT INTO dup (missing) VALUES (1)")


if __name__ == "__main__":
    unittest.main()
