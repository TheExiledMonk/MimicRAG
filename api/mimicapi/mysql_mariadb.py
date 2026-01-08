from __future__ import annotations

from dataclasses import dataclass
import re
import time

from .api_client import ApiClient


_TYPE_MAP = {
    "INT": "int32",
    "INTEGER": "int32",
    "BIGINT": "int64",
    "FLOAT": "float64",
    "DOUBLE": "float64",
    "REAL": "float64",
    "BOOLEAN": "bool",
    "BOOL": "bool",
    "TINYINT": "bool",
    "VARCHAR": "string",
    "TEXT": "string",
    "CHAR": "string",
    "BLOB": "bytes",
    "VARBINARY": "bytes",
    "DATETIME": "string",
    "TIMESTAMP": "string",
}


class SQLExecutionError(Exception):
    pass


class SQLIntegrityError(SQLExecutionError):
    pass


class SQLWarning(UserWarning):
    pass


@dataclass
class CursorResult:
    rows: list[tuple]
    description: list[tuple] | None
    rowcount: int
    lastrowid: int | None


class MySQLConnection:
    def __init__(
        self,
        api_client: ApiClient | None = None,
        host: str | None = None,
        port: int | None = None,
        user: str | None = None,
        password: str | None = None,
        ssl: object | None = None,
        database: str = "default",
    ) -> None:
        if api_client is None:
            api_client = ApiClient()
            api_client.add_mimicdb_backend("primary", host=host, port=port, default_db=database)
        self._client = api_client
        self._db = database
        self._user = user
        self._password = password
        self._ssl = ssl
        self._schemas: dict[tuple[str, str], list[tuple[str, str]]] = {}
        self._id_counters: dict[tuple[str, str], int] = {}
        self._version_counter = 0
        self._session_vars: dict[str, object] = {}
        self._plan_cache: dict[str, tuple[str, tuple]] = {}

    def cursor(self) -> "MySQLCursor":
        return MySQLCursor(self)

    def close(self) -> None:
        return None

    def commit(self) -> None:
        return None

    def rollback(self) -> None:
        return None

    def session_vars(self) -> dict[str, object]:
        return dict(self._session_vars)

    def set_session_var(self, name: str, value: object) -> None:
        self._session_vars[name.upper()] = value

    def get_session_var(self, name: str) -> object | None:
        return self._session_vars.get(name.upper())

    def use_database(self, name: str) -> None:
        self._db = name
        self._client.create_database_all(name)

    def _next_version(self) -> int:
        self._version_counter += 1
        return (time.time_ns() & 0xFFFFFFFFFFFFFFFF) ^ self._version_counter


class MySQLCursor:
    def __init__(self, connection: MySQLConnection) -> None:
        self._conn = connection
        self._result = CursorResult([], None, -1, None)
        self._warnings: list[str] = []

    @property
    def rowcount(self) -> int:
        return self._result.rowcount

    @property
    def lastrowid(self) -> int | None:
        return self._result.lastrowid

    @property
    def description(self):
        return self._result.description

    @property
    def warnings(self) -> list[str]:
        return list(self._warnings)

    def execute(self, query: str, params=None) -> int:
        self._warnings = []
        if params:
            query = _apply_params(query, params)
        query = query.strip().rstrip(";")
        cached = self._conn._plan_cache.get(query)
        if cached is not None:
            return self._execute_plan(cached)
        try:
            plan = _compile_query_plan(query)
            self._conn._plan_cache[query] = plan
            return self._execute_plan(plan)
        except SQLExecutionError:
            pass
        upper = query.upper()
        if upper.startswith("SET "):
            name, value = _parse_set(query)
            self._conn.set_session_var(name, value)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("SHOW VARIABLES"):
            rows = [
                (name, value)
                for name, value in sorted(self._conn._session_vars.items())
            ]
            self._result = CursorResult(rows, [("Variable_name", None, None, None, None, None, None),
                                               ("Value", None, None, None, None, None, None)], len(rows), None)
            return len(rows)
        if upper.startswith("SHOW DATABASES"):
            databases = self._conn._client.list_databases_all()
            rows = [(name,) for name in databases]
            self._result = CursorResult(rows, [("Database", None, None, None, None, None, None)], len(rows), None)
            return len(rows)
        if upper.startswith("SHOW TABLES"):
            rows = [
                (name,)
                for (db, name) in sorted(self._conn._schemas.keys())
                if db == self._conn._db
            ]
            self._result = CursorResult(rows, [("Tables_in_db", None, None, None, None, None, None)], len(rows), None)
            return len(rows)
        if upper.startswith("DESCRIBE") or upper.startswith("SHOW COLUMNS"):
            table = _parse_identifier(query.split()[-1])
            fields = self._schema_for(table)
            rows = [(name, ftype) for name, ftype in fields]
            self._result = CursorResult(rows, [("Field", None, None, None, None, None, None),
                                               ("Type", None, None, None, None, None, None)], len(rows), None)
            return len(rows)
        if upper.startswith("CREATE DATABASE"):
            name = _parse_identifier(query.split()[-1])
            self._conn._client.create_database_all(name)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("USE "):
            name = _parse_identifier(query.split()[1])
            self._conn.use_database(name)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("CREATE TABLE"):
            table, columns = _parse_create_table(query)
            fields = [("_id", "int64"), ("_deleted", "bool"), ("_version", "int64")]
            for name, sql_type in columns:
                fields.append((name, _map_type(sql_type)))
            key = (self._conn._db, table)
            self._conn._schemas[key] = fields
            self._conn._client.create_dataset_all(self._conn._db, table, fields)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("ALTER TABLE"):
            table, name, sql_type = _parse_alter_add(query)
            fields = list(self._schema_for(table))
            field_names = {fname for fname, _ in fields}
            if name in field_names:
                raise SQLExecutionError(f"column '{name}' already exists")
            fields.append((name, _map_type(sql_type)))
            self._conn._schemas[(self._conn._db, table)] = fields
            self._conn._client.create_dataset_all(self._conn._db, table, fields)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("DROP TABLE"):
            table = _parse_identifier(query.split()[-1])
            self._conn._schemas.pop((self._conn._db, table), None)
            self._result = CursorResult([], None, 0, None)
            return 0
        if upper.startswith("REPLACE INTO"):
            table, columns, rows, _, _ = _parse_insert(query, replace=True)
            self._replace_rows(table, columns, rows)
            return self._result.rowcount
        if upper.startswith("INSERT"):
            table, columns, rows, ignore, on_duplicate = _parse_insert(query)
            if on_duplicate:
                self._upsert_rows(table, columns, rows, on_duplicate)
            else:
                self._insert_rows(table, columns, rows, ignore=ignore)
            return self._result.rowcount
        if upper.startswith("SELECT"):
            self._select_query(query)
            return self._result.rowcount
        if upper.startswith("UPDATE"):
            self._update_query(query)
            return self._result.rowcount
        if upper.startswith("DELETE FROM"):
            self._delete_query(query)
            return self._result.rowcount
        raise SQLExecutionError("unsupported query")

    def _execute_plan(self, plan: tuple[str, tuple]) -> int:
        op_name, payload = plan
        if op_name == "INSERT":
            table, columns, rows, ignore, on_duplicate = payload
            if on_duplicate:
                self._upsert_rows(table, columns, rows, on_duplicate)
            else:
                self._insert_rows(table, columns, rows, ignore=ignore)
            return self._result.rowcount
        if op_name == "REPLACE":
            table, columns, rows = payload
            self._replace_rows(table, columns, rows)
            return self._result.rowcount
        if op_name == "SELECT":
            self._select_query_parsed(*payload)
            return self._result.rowcount
        if op_name == "UPDATE":
            table, assignments, where = payload
            self._update_query_parsed(table, assignments, where)
            return self._result.rowcount
        if op_name == "DELETE":
            table, where = payload
            self._delete_query_parsed(table, where)
            return self._result.rowcount
        raise SQLExecutionError("cached op unsupported")

    def fetchone(self):
        if not self._result.rows:
            return None
        return self._result.rows.pop(0)

    def fetchall(self):
        rows = list(self._result.rows)
        self._result.rows = []
        return rows

    def _schema_for(self, table: str) -> list[tuple[str, str]]:
        key = (self._conn._db, table)
        if key not in self._conn._schemas:
            raise SQLExecutionError(f"unknown table '{table}'")
        return self._conn._schemas[key]

    def _insert_rows(self, table: str, columns: list[str], rows: list[list], ignore: bool = False) -> None:
        fields = self._schema_for(table)
        field_names = [name for name, _ in fields]
        if not columns:
            columns = [name for name in field_names if name not in ("_id", "_deleted", "_version")]
        columns_set = set(columns)
        missing = columns_set - set(field_names)
        if missing:
            raise SQLExecutionError(f"unknown columns: {sorted(missing)}")
        existing = {row.get("_id") for row in _latest_rows(self._conn._client, self._conn._db, table, fields)}
        columns_data = {name: [] for name in field_names}
        last_id = None
        inserted = 0
        for row in rows:
            if len(row) != len(columns):
                raise SQLExecutionError("column/value count mismatch")
            record = dict(zip(columns, row))
            if "_id" in record and record["_id"] is not None:
                doc_id = int(record["_id"])
            else:
                doc_id = self._next_id(table)
            if doc_id in existing:
                if ignore:
                    self._warnings.append(f"duplicate _id {doc_id} ignored")
                    continue
                raise SQLIntegrityError(f"duplicate _id {doc_id}")
            record["_id"] = doc_id
            record["_deleted"] = False
            record["_version"] = self._conn._next_version()
            last_id = doc_id
            existing.add(doc_id)
            for name in field_names:
                columns_data[name].append(record.get(name))
            inserted += 1
        self._conn._client.append_batch_fanout(
            self._conn._db,
            table,
            fields,
            columns_data,
        )
        self._result = CursorResult([], None, inserted, last_id)

    def _replace_rows(self, table: str, columns: list[str], rows: list[list]) -> None:
        fields = self._schema_for(table)
        field_names = [name for name, _ in fields]
        if not columns:
            columns = [name for name in field_names if name not in ("_id", "_deleted", "_version")]
        columns_set = set(columns)
        missing = columns_set - set(field_names)
        if missing:
            raise SQLExecutionError(f"unknown columns: {sorted(missing)}")
        existing_rows = _latest_rows(self._conn._client, self._conn._db, table, fields)
        existing_by_id = {row.get("_id"): row for row in existing_rows}
        columns_data = {name: [] for name in field_names}
        last_id = None
        modified = 0
        for row in rows:
            if len(row) != len(columns):
                raise SQLExecutionError("column/value count mismatch")
            record = dict(zip(columns, row))
            if "_id" in record and record["_id"] is not None:
                doc_id = int(record["_id"])
            else:
                doc_id = self._next_id(table)
            record["_id"] = doc_id
            record["_deleted"] = False
            record["_version"] = self._conn._next_version()
            last_id = doc_id
            if doc_id in existing_by_id:
                tombstone = dict(existing_by_id[doc_id])
                tombstone["_deleted"] = True
                tombstone["_version"] = self._conn._next_version()
                for name in field_names:
                    columns_data[name].append(tombstone.get(name))
                modified += 1
            for name in field_names:
                columns_data[name].append(record.get(name))
            modified += 1
        self._conn._client.append_batch_fanout(
            self._conn._db,
            table,
            fields,
            columns_data,
        )
        self._result = CursorResult([], None, modified, last_id)

    def _upsert_rows(
        self,
        table: str,
        columns: list[str],
        rows: list[list],
        assignments: dict[str, str],
    ) -> None:
        fields = self._schema_for(table)
        field_names = [name for name, _ in fields]
        if not columns:
            columns = [name for name in field_names if name not in ("_id", "_deleted", "_version")]
        columns_set = set(columns)
        missing = columns_set - set(field_names)
        if missing:
            raise SQLExecutionError(f"unknown columns: {sorted(missing)}")
        existing_rows = _latest_rows(self._conn._client, self._conn._db, table, fields)
        existing_by_id = {row.get("_id"): row for row in existing_rows}
        columns_data = {name: [] for name in field_names}
        last_id = None
        modified = 0
        for row in rows:
            if len(row) != len(columns):
                raise SQLExecutionError("column/value count mismatch")
            record = dict(zip(columns, row))
            if "_id" in record and record["_id"] is not None:
                doc_id = int(record["_id"])
            else:
                doc_id = self._next_id(table)
            record["_id"] = doc_id
            current = existing_by_id.get(doc_id)
            if current is None:
                record["_deleted"] = False
                record["_version"] = self._conn._next_version()
                last_id = doc_id
                for name in field_names:
                    columns_data[name].append(record.get(name))
                modified += 1
                continue
            updated = dict(current)
            for key, value in assignments.items():
                if value.upper().startswith("VALUES(") and value.endswith(")"):
                    col_name = value[7:-1].strip()
                    updated[key] = record.get(_parse_identifier(col_name))
                else:
                    updated[key] = _parse_value(value)
            updated["_deleted"] = False
            updated["_version"] = self._conn._next_version()
            last_id = doc_id
            for name in field_names:
                columns_data[name].append(updated.get(name))
            modified += 1
        self._conn._client.append_batch_fanout(
            self._conn._db,
            table,
            fields,
            columns_data,
        )
        self._result = CursorResult([], None, modified, last_id)

    def _select_query(self, query: str) -> None:
        parsed = _parse_select(query)
        self._select_query_parsed(*parsed)

    def _select_query_parsed(
        self,
        table: str,
        columns: list[str],
        where,
        order_by,
        limit,
        offset,
        group_by,
        having,
        joins,
    ) -> None:
        fields = self._schema_for(table)
        rows = _latest_rows(
            self._conn._client,
            self._conn._db,
            table,
            fields,
        )
        rows = _apply_joins(
            rows,
            table,
            joins,
            self._conn._db,
            self._conn._client,
            self._conn._schemas,
        )
        if where:
            rows = [row for row in rows if _match_where(row, where)]
        if group_by:
            rows = _apply_group_by(rows, group_by, columns, having)
        if order_by:
            rows = _sort_rows(rows, order_by)
        if offset:
            rows = rows[offset:]
        if limit is not None:
            rows = rows[:limit]
        if columns == ["*"]:
            output_cols = [name for name, _ in fields if name not in ("_deleted", "_version")]
        else:
            output_cols = columns
        results = [tuple(row.get(col) for col in output_cols) for row in rows]
        description = [(col, None, None, None, None, None, None) for col in output_cols]
        self._result = CursorResult(results, description, len(results), None)

    def _update_query(self, query: str) -> None:
        table, assignments, where = _parse_update(query)
        self._update_query_parsed(table, assignments, where)

    def _delete_query(self, query: str) -> None:
        table, where = _parse_delete(query)
        self._delete_query_parsed(table, where)

    def _next_id(self, table: str) -> int:
        key = (self._conn._db, table)
        next_id = self._conn._id_counters.get(key, 1)
        self._conn._id_counters[key] = next_id + 1
        return next_id

    def _update_query_parsed(self, table: str, assignments: dict[str, str], where) -> None:
        fields = self._schema_for(table)
        rows = _latest_rows(
            self._conn._client,
            self._conn._db,
            table,
            fields,
        )
        if where:
            rows = [row for row in rows if _match_where(row, where)]
        if not rows:
            self._result = CursorResult([], None, 0, None)
            return
        field_names = [name for name, _ in fields]
        columns_data = {name: [] for name in field_names}
        for row in rows:
            updated = dict(row)
            for key, value in assignments.items():
                updated[key] = _parse_value(value)
            updated["_version"] = self._conn._next_version()
            for name in field_names:
                columns_data[name].append(updated.get(name))
        self._conn._client.append_batch_fanout(
            self._conn._db,
            table,
            fields,
            columns_data,
        )
        self._result = CursorResult([], None, len(rows), None)

    def _delete_query_parsed(self, table: str, where) -> None:
        fields = self._schema_for(table)
        rows = _latest_rows(
            self._conn._client,
            self._conn._db,
            table,
            fields,
        )
        if where:
            rows = [row for row in rows if _match_where(row, where)]
        if not rows:
            self._result = CursorResult([], None, 0, None)
            return
        field_names = [name for name, _ in fields]
        columns_data = {name: [] for name in field_names}
        for row in rows:
            deleted = dict(row)
            deleted["_deleted"] = True
            deleted["_version"] = self._conn._next_version()
            for name in field_names:
                columns_data[name].append(deleted.get(name))
        self._conn._client.append_batch_fanout(
            self._conn._db,
            table,
            fields,
            columns_data,
        )
        self._result = CursorResult([], None, len(rows), None)


def _compile_query_plan(query: str) -> tuple[str, tuple]:
    stripped = query.strip().rstrip(";")
    upper = stripped.upper()
    if upper.startswith("INSERT"):
        table, columns, rows, ignore, on_duplicate = _parse_insert(stripped)
        return "INSERT", (table, columns, rows, ignore, on_duplicate)
    if upper.startswith("REPLACE"):
        table, columns, rows, _, _ = _parse_insert(stripped, replace=True)
        return "REPLACE", (table, columns, rows)
    if upper.startswith("SELECT"):
        return "SELECT", _parse_select(stripped)
    if upper.startswith("UPDATE"):
        table, assignments, where = _parse_update(stripped)
        return "UPDATE", (table, assignments, where)
    if upper.startswith("DELETE"):
        table, where = _parse_delete(stripped)
        return "DELETE", (table, where)
    raise SQLExecutionError("unsupported plan")


def _strip_meta(row: dict) -> dict:
    out = dict(row)
    out.pop("_deleted", None)
    out.pop("_version", None)
    return out


def connect(*args, **kwargs) -> MySQLConnection:
    return MySQLConnection(*args, **kwargs)


class ConnectionPool:
    def __init__(
        self,
        host: str | None = None,
        port: int | None = None,
        user: str | None = None,
        password: str | None = None,
        ssl: object | None = None,
        database: str = "default",
    ) -> None:
        self._host = host
        self._port = port
        self._user = user
        self._password = password
        self._ssl = ssl
        self._database = database

    def connect(self) -> MySQLConnection:
        return MySQLConnection(
            host=self._host,
            port=self._port,
            user=self._user,
            password=self._password,
            ssl=self._ssl,
            database=self._database,
        )


def _apply_params(query: str, params) -> str:
    if isinstance(params, dict):
        for key, value in params.items():
            query = query.replace(f"%({key})s", _format_value(value))
        return query
    parts = query.split("?")
    if not isinstance(params, (list, tuple)):
        params = [params]
    if len(parts) - 1 != len(params):
        raise SQLExecutionError("parameter count mismatch")
    out = []
    for part, value in zip(parts, params):
        out.append(part)
        out.append(_format_value(value))
    out.append(parts[-1])
    return "".join(out)


def _format_value(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    if isinstance(value, bytes):
        return "0x" + value.hex()
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


def _parse_identifier(raw: str) -> str:
    return raw.strip("`")


def _map_type(sql_type: str) -> str:
    base = sql_type.split("(")[0].strip().upper()
    return _TYPE_MAP.get(base, "string")


def _parse_set(query: str) -> tuple[str, object]:
    match = re.match(r"SET\s+(.+)", query, re.IGNORECASE | re.DOTALL)
    if not match:
        raise SQLExecutionError("invalid SET statement")
    part = match.group(1).strip()
    if part.upper().startswith("SESSION "):
        part = part[8:].strip()
    if "=" not in part:
        raise SQLExecutionError("invalid SET assignment")
    name, value = part.split("=", 1)
    return _parse_identifier(name.strip()), _parse_value(value.strip())


def _split_csv(text: str) -> list[str]:
    parts = []
    current = []
    depth = 0
    in_string = False
    it = iter(text)
    for char in it:
        if char == "'" and not in_string:
            in_string = True
            current.append(char)
            continue
        if char == "'" and in_string:
            in_string = False
            current.append(char)
            continue
        if in_string:
            current.append(char)
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        if char == "," and depth == 0:
            parts.append("".join(current).strip())
            current = []
            continue
        current.append(char)
    if current:
        parts.append("".join(current).strip())
    return parts


def _parse_create_table(query: str) -> tuple[str, list[tuple[str, str]]]:
    match = re.match(r"CREATE\s+TABLE\s+`?(\w+)`?\s*\((.+)\)", query, re.IGNORECASE | re.DOTALL)
    if not match:
        raise SQLExecutionError("invalid CREATE TABLE")
    table = match.group(1)
    cols = _split_csv(match.group(2))
    out = []
    for col in cols:
        parts = col.strip().split()
        if len(parts) < 2:
            continue
        name = _parse_identifier(parts[0])
        col_type = parts[1]
        out.append((name, col_type))
    return table, out


def _parse_alter_add(query: str) -> tuple[str, str, str]:
    match = re.match(
        r"ALTER\s+TABLE\s+`?(\w+)`?\s+ADD\s+COLUMN\s+`?(\w+)`?\s+(\w+\(?[^\s)]*\)?)",
        query,
        re.IGNORECASE | re.DOTALL,
    )
    if not match:
        raise SQLExecutionError("invalid ALTER TABLE")
    table = match.group(1)
    name = match.group(2)
    sql_type = match.group(3)
    return table, name, sql_type


def _parse_insert(query: str, replace: bool = False) -> tuple[str, list[str], list[list], bool, dict[str, str]]:
    ignore = False
    on_duplicate: dict[str, str] = {}
    if not replace and re.match(r"INSERT\s+IGNORE", query, re.IGNORECASE):
        ignore = True
    match = re.match(
        r"(INSERT|REPLACE)\s+(IGNORE\s+)?INTO\s+`?(\w+)`?\s*(\([^)]*\))?\s*VALUES\s*(.+)",
        query,
        re.IGNORECASE | re.DOTALL,
    )
    if not match:
        raise SQLExecutionError("invalid INSERT/REPLACE")
    table = match.group(3)
    cols_raw = match.group(4)
    columns = []
    if cols_raw:
        columns = [_parse_identifier(col) for col in _split_csv(cols_raw.strip()[1:-1])]
    values_raw = match.group(5).strip()
    on_dup_index = values_raw.upper().find("ON DUPLICATE KEY UPDATE")
    if on_dup_index != -1:
        assignments = values_raw[on_dup_index + len("ON DUPLICATE KEY UPDATE"):].strip()
        values_raw = values_raw[:on_dup_index].strip()
        on_duplicate = _parse_assignments(assignments)
    rows = []
    for group in _extract_groups(values_raw):
        rows.append([_parse_value(token) for token in _split_csv(group)])
    return table, columns, rows, ignore, on_duplicate


def _extract_groups(text: str) -> list[str]:
    groups = []
    depth = 0
    current = []
    in_string = False
    for char in text:
        if char == "'" and not in_string:
            in_string = True
        elif char == "'" and in_string:
            in_string = False
        if char == "(" and not in_string:
            if depth == 0:
                current = []
            depth += 1
            continue
        if char == ")" and not in_string:
            depth -= 1
            if depth == 0:
                groups.append("".join(current))
                continue
        if depth >= 1:
            current.append(char)
    return groups


def _parse_select(query: str):
    match = re.match(r"SELECT\s+(.+)\s+FROM\s+`?(\w+)`?(.*)", query, re.IGNORECASE | re.DOTALL)
    if not match:
        raise SQLExecutionError("invalid SELECT")
    columns_raw = match.group(1).strip()
    table = match.group(2)
    tail = match.group(3)
    columns = ["*"] if columns_raw == "*" else [_parse_identifier(col) for col in _split_csv(columns_raw)]
    join_clause, tail = _split_join_clause(tail)
    joins = _parse_joins(join_clause, table)
    where = _extract_clause(tail, "WHERE")
    order_by = _extract_clause(tail, "ORDER BY")
    group_by = _extract_clause(tail, "GROUP BY")
    having = _extract_clause(tail, "HAVING")
    limit = _extract_clause(tail, "LIMIT")
    offset = _extract_clause(tail, "OFFSET")
    parsed_where = _parse_where(where) if where else []
    parsed_order = _parse_order(order_by) if order_by else []
    parsed_group = _parse_group_by(group_by) if group_by else []
    parsed_having = _parse_having(having) if having else []
    parsed_limit = _parse_limit(limit)
    parsed_offset = int(offset) if offset else 0
    return table, columns, parsed_where, parsed_order, parsed_limit, parsed_offset, parsed_group, parsed_having, joins


def _parse_update(query: str):
    match = re.match(r"UPDATE\s+`?(\w+)`?\s+SET\s+(.+)", query, re.IGNORECASE | re.DOTALL)
    if not match:
        raise SQLExecutionError("invalid UPDATE")
    table = match.group(1)
    remainder = match.group(2)
    where_clause = _extract_clause(remainder, "WHERE")
    set_part = remainder
    if where_clause:
        set_part = remainder.split("WHERE")[0]
    assignments = _parse_assignments(set_part)
    where = _parse_where(where_clause) if where_clause else []
    return table, assignments, where


def _parse_delete(query: str):
    match = re.match(r"DELETE\s+FROM\s+`?(\w+)`?(.*)", query, re.IGNORECASE | re.DOTALL)
    if not match:
        raise SQLExecutionError("invalid DELETE")
    table = match.group(1)
    tail = match.group(2)
    where = _extract_clause(tail, "WHERE")
    return table, _parse_where(where) if where else []


def _parse_assignments(text: str) -> dict[str, str]:
    assignments = {}
    for part in _split_csv(text):
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        assignments[_parse_identifier(key.strip())] = value.strip()
    return assignments


def _extract_clause(text: str, keyword: str) -> str | None:
    upper = text.upper()
    keyword = keyword.upper()
    if keyword not in upper:
        return None
    start = upper.index(keyword)
    clause = text[start + len(keyword):]
    stop_keywords = [" WHERE ", " GROUP BY ", " HAVING ", " ORDER BY ", " LIMIT ", " OFFSET "]
    end = None
    for stop in stop_keywords:
        idx = clause.upper().find(stop)
        if idx != -1:
            end = idx
            break
    if end is not None:
        clause = clause[:end]
    return clause.strip()


def _parse_where(where: str) -> list[tuple[str, str, object]]:
    if not where:
        return []
    parts = [part.strip() for part in where.split("AND")]
    out = []
    for part in parts:
        upper = part.upper()
        if " BETWEEN " in upper:
            left, right = part.split("BETWEEN", 1)
            col = _parse_identifier(left.strip())
            range_part = right.split("AND")
            if len(range_part) != 2:
                raise SQLExecutionError("invalid BETWEEN")
            out.append((col, ">=", _parse_value(range_part[0].strip())))
            out.append((col, "<=", _parse_value(range_part[1].strip())))
            continue
        if " IN " in upper:
            col, rest = part.split("IN", 1)
            col = _parse_identifier(col.strip())
            values = [_parse_value(val) for val in _split_csv(rest.strip()[1:-1])]
            out.append((col, "IN", values))
            continue
        if " LIKE " in upper:
            col, pattern = part.split("LIKE", 1)
            out.append((_parse_identifier(col.strip()), "LIKE", _parse_value(pattern.strip())))
            continue
        if " IS NULL" in upper:
            col = part.split("IS")[0].strip()
            out.append((_parse_identifier(col), "IS", None))
            continue
        if " IS NOT NULL" in upper:
            col = part.split("IS")[0].strip()
            out.append((_parse_identifier(col), "IS NOT", None))
            continue
        for op in ("<=", ">=", "<>", "!=", "=", "<", ">"):
            if op in part:
                left, right = part.split(op, 1)
                out.append((_parse_identifier(left.strip()), op, _parse_value(right.strip())))
                break
    return out


def _parse_order(text: str) -> list[tuple[str, int]]:
    if not text:
        return []
    entries = _split_csv(text)
    out = []
    for entry in entries:
        parts = entry.split()
        name = _parse_identifier(parts[0])
        direction = 1
        if len(parts) > 1 and parts[1].upper() == "DESC":
            direction = -1
        out.append((name, direction))
    return out


def _split_join_clause(tail: str) -> tuple[str, str]:
    upper = tail.upper()
    stop_keywords = [" WHERE ", " GROUP BY ", " HAVING ", " ORDER BY ", " LIMIT ", " OFFSET "]
    end = None
    for stop in stop_keywords:
        idx = upper.find(stop)
        if idx != -1:
            end = idx
            break
    if end is None:
        return tail.strip(), ""
    return tail[:end].strip(), tail[end:].strip()


def _parse_joins(text: str, base_table: str) -> list[tuple[str, str, str, str]]:
    if not text:
        return []
    joins = []
    pattern = re.compile(
        r"(INNER|LEFT)?\s*JOIN\s+`?(\w+)`?\s+ON\s+([\w\.]+)\s*=\s*([\w\.]+)",
        re.IGNORECASE,
    )
    for match in pattern.finditer(text):
        join_type = (match.group(1) or "INNER").upper()
        table = match.group(2)
        left_key = match.group(3)
        right_key = match.group(4)
        joins.append((join_type, table, left_key, right_key))
    return joins


def _parse_group_by(text: str) -> list[str]:
    if not text:
        return []
    return [_parse_identifier(part) for part in _split_csv(text)]


def _parse_having(text: str) -> list[tuple[str, str, object]]:
    if not text:
        return []
    clauses = []
    for part in text.split("AND"):
        part = part.strip()
        for op in ("<=", ">=", "<>", "!=", "=", "<", ">"):
            if op in part:
                left, right = part.split(op, 1)
                clauses.append((left.strip(), op, _parse_value(right.strip())))
                break
    return clauses


def _parse_limit(text: str) -> int | None:
    if not text:
        return None
    if "," in text:
        parts = [p.strip() for p in text.split(",", 1)]
        return int(parts[1])
    return int(text.strip())


def _parse_value(raw: str):
    raw = raw.strip()
    if raw.upper() == "NULL":
        return None
    if raw.startswith("'") and raw.endswith("'"):
        text = raw[1:-1].replace("''", "'")
        return text
    if raw.startswith("0x"):
        return bytes.fromhex(raw[2:])
    try:
        if "." in raw:
            return float(raw)
        return int(raw)
    except ValueError:
        return raw


def _latest_rows(client: ApiClient, db: str, table: str, fields: list[tuple[str, str]]):
    rows, _ = client.scan_routed(db, table, fields, columns=[name for name, _ in fields])
    latest: dict[int, dict] = {}
    for row in rows:
        row_id = row.get("_id")
        if row_id is None:
            continue
        version = row.get("_version", 0)
        current = latest.get(row_id)
        if current is None or version > current.get("_version", 0):
            latest[row_id] = row
    return [_strip_meta(row) for row in latest.values() if not row.get("_deleted")]


def _match_where(row: dict, clauses: list[tuple[str, str, object]]) -> bool:
    for col, op, value in clauses:
        current = row.get(col)
        if op == "IN":
            if current not in value:
                return False
        elif op == "LIKE":
            pattern = "^" + re.escape(str(value)).replace("%", ".*").replace("_", ".") + "$"
            if current is None or not re.match(pattern, str(current)):
                return False
        elif op == "IS":
            if current is not None:
                return False
        elif op == "IS NOT":
            if current is None:
                return False
        else:
            if current is None:
                return False
            if op == "=" and current != value:
                return False
            if op in ("!=", "<>") and current == value:
                return False
            if op == "<" and current >= value:
                return False
            if op == "<=" and current > value:
                return False
            if op == ">" and current <= value:
                return False
            if op == ">=" and current < value:
                return False
    return True


def _sort_rows(rows: list[dict], order_by: list[tuple[str, int]]) -> list[dict]:
    def sort_key(row):
        return tuple(row.get(name) for name, _ in order_by)
    reverse = any(direction < 0 for _, direction in order_by)
    return sorted(rows, key=sort_key, reverse=reverse)


def _apply_joins(
    base_rows: list[dict],
    base_table: str,
    joins: list[tuple[str, str, str, str]],
    db: str,
    client: ApiClient,
    schemas: dict[tuple[str, str], list[tuple[str, str]]],
) -> list[dict]:
    if not joins:
        return [_prefix_row(base_table, row) for row in base_rows]
    rows = [_prefix_row(base_table, row) for row in base_rows]
    for join_type, table, left_key, right_key in joins:
        fields = schemas.get((db, table))
        if fields is None:
            raise SQLExecutionError(f"unknown table '{table}'")
        join_rows = _latest_rows(client, db, table, fields)
        join_rows_prefixed = [_prefix_row(table, row) for row in join_rows]
        merged = []
        for row in rows:
            matched = False
            for join_row in join_rows_prefixed:
                left_value = _lookup_join_value(row, left_key)
                right_value = _lookup_join_value(join_row, right_key)
                if left_value == right_value:
                    merged.append(_merge_rows(row, join_row))
                    matched = True
            if not matched and join_type == "LEFT":
                merged.append(dict(row))
        rows = merged
    return rows


def _prefix_row(table: str, row: dict) -> dict:
    out = {}
    for key, value in row.items():
        qualified = f"{table}.{key}"
        out[qualified] = value
        if key not in out:
            out[key] = value
    return out


def _merge_rows(base: dict, join_row: dict) -> dict:
    merged = dict(base)
    for key, value in join_row.items():
        if key in merged:
            continue
        merged[key] = value
    return merged


def _lookup_join_value(row: dict, key: str):
    return row.get(key)


def _apply_group_by(
    rows: list[dict],
    group_by: list[str],
    columns: list[str],
    having: list[tuple[str, str, object]],
) -> list[dict]:
    groups: dict[tuple, dict] = {}
    for row in rows:
        key = tuple(row.get(name) for name in group_by)
        if key not in groups:
            agg = {"__rows": 0}
            for col in columns:
                if col.upper().startswith("COUNT("):
                    agg[col] = 0
                elif col.upper().startswith("SUM("):
                    agg[col] = 0.0
                elif col.upper().startswith("MIN("):
                    agg[col] = None
                elif col.upper().startswith("MAX("):
                    agg[col] = None
            groups[key] = agg
        agg = groups[key]
        agg["__rows"] += 1
        for col in columns:
            upper = col.upper()
            if upper.startswith("COUNT("):
                agg[col] += 1
            elif upper.startswith("SUM("):
                field = col[col.find("(") + 1:col.rfind(")")].strip()
                value = row.get(field)
                if value is not None:
                    agg[col] += float(value)
            elif upper.startswith("MIN("):
                field = col[col.find("(") + 1:col.rfind(")")].strip()
                value = row.get(field)
                if value is not None:
                    if agg[col] is None or value < agg[col]:
                        agg[col] = value
            elif upper.startswith("MAX("):
                field = col[col.find("(") + 1:col.rfind(")")].strip()
                value = row.get(field)
                if value is not None:
                    if agg[col] is None or value > agg[col]:
                        agg[col] = value
    result_rows = []
    for key, agg in groups.items():
        out = {}
        for idx, name in enumerate(group_by):
            out[name] = key[idx]
        for col in columns:
            upper = col.upper()
            if upper.startswith(("COUNT(", "SUM(", "MIN(", "MAX(")):
                out[col] = agg.get(col)
        if _match_having(out, having):
            result_rows.append(out)
    return result_rows


def _match_having(row: dict, clauses: list[tuple[str, str, object]]) -> bool:
    if not clauses:
        return True
    for key, op, value in clauses:
        current = row.get(key)
        if current is None:
            return False
        if op == "=" and current != value:
            return False
        if op in ("!=", "<>") and current == value:
            return False
        if op == "<" and current >= value:
            return False
        if op == "<=" and current > value:
            return False
        if op == ">" and current <= value:
            return False
        if op == ">=" and current < value:
            return False
    return True
