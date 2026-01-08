import argparse
import random
import subprocess
import time
from pathlib import Path
import sys

from mimicapi import ApiClient, Dataset, MongoClientCpp, MySQLConnection, CppApiClient
from mimicapi.sql_parser import parse_sql
from mimicapi.sql_exec import execute_query


def resolve_server_bin(server_bin: str, repo_root: Path) -> str:
    candidate = Path(server_bin)
    if candidate.exists():
        return str(candidate)
    fallback = [
        repo_root / "build" / "mimicdb_server",
        repo_root / "build" / "server" / "mimicdb_server",
        repo_root / "mimicdb_server",
    ]
    for path in fallback:
        if path.exists():
            return str(path)
    raise FileNotFoundError(
        f"server binary not found at '{server_bin}'. "
        "Build the server or pass the correct path via --server-bin."
    )


def build_batch(rng: random.Random, count: int) -> dict[str, list]:
    ages = []
    countries = []
    incomes = []
    for _ in range(count):
        ages.append(rng.randint(18, 90))
        countries.append(rng.randint(1, 200))
        income = float(rng.randint(10_000, 150_000))
        if rng.random() < 0.05:
            income = None
        incomes.append(income)
    return {"age": ages, "country": countries, "income": incomes}


def run_dataset_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    backend: str,
    host: str | None,
    port: int | None,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    users = Dataset(
        name="users",
        fields={
            "age": "int32",
            "country": "int32",
            "income": "float64",
        },
        use_cpp=backend == "cpp",
        host=host,
        port=port,
        database=database,
    )
    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            users.append(
                age=batch["age"][0],
                country=batch["country"][0],
                income=batch["income"][0],
            )
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            users.append_batch(build_batch(rng, current))
            remaining -= current
    append_end = time.perf_counter()

    result = users.filter(age_gt=30, country_eq=45).aggregate(
        sum="income", count=True, min="income", max="income"
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


def run_mimicapi_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    client = ApiClient()
    client.add_mimicdb_backend("primary", host=host, port=port, default_db=database)
    fields = [
        ("age", "int32"),
        ("country", "int32"),
        ("income", "float64"),
    ]
    client.create_database_all(database)
    client.create_dataset_all(database, "users", fields)

    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            client.append_batch_fanout(database, "users", fields, batch)
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            client.append_batch_fanout(database, "users", fields, build_batch(rng, current))
            remaining -= current
    append_end = time.perf_counter()

    result, _ = client.query_agg_routed(
        database,
        "users",
        field_index=2,
        predicates=[(0, 4, 30.0), (1, 0, 45.0)],
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


def run_mongo_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    _ = host
    _ = port
    mongo = MongoClientCpp()
    db = database
    collection = mongo

    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            collection.insert_one(
                db,
                "users",
                {
                    "age": batch["age"][0],
                    "country": batch["country"][0],
                    "income": batch["income"][0],
                },
            )
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            batch = build_batch(rng, current)
            docs = [
                {"age": age, "country": country, "income": income}
                for age, country, income in zip(
                    batch["age"], batch["country"], batch["income"]
                )
            ]
            collection.insert_many(db, "users", docs)
            remaining -= current
    append_end = time.perf_counter()

    result = collection.aggregate(
        db,
        "users",
        [
            {"$match": {"age": {"$gt": 30}, "country": 45}},
            {
                "$group": {
                    "_id": None,
                    "sum": {"$sum": "$income"},
                    "count": {"$sum": 1},
                    "min": {"$min": "$income"},
                    "max": {"$max": "$income"},
                }
            },
        ],
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


def run_cpp_core_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    client = CppApiClient()
    client.create_database(database)
    client.create_dataset(
        database,
        "users",
        {
            "age": "int32",
            "country": "int32",
            "income": "float64",
        },
    )
    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            client.append_batch(
                database,
                "users",
                {
                    "age": batch["age"],
                    "country": batch["country"],
                    "income": batch["income"],
                },
            )
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            client.append_batch(database, "users", build_batch(rng, current))
            remaining -= current
    append_end = time.perf_counter()

    result = client.aggregate(
        database,
        "users",
        field_index=2,
        predicates=[(0, "gt", 30.0), (1, "eq", 45.0)],
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


class SqlCppClient:
    def __init__(self, core: CppApiClient) -> None:
        self._core = core

    def scan_routed(self, db, dataset, fields, columns=None, predicates=None, limit=0, offset=0):
        op_map = {0: "eq", 1: "ne", 2: "lt", 3: "le", 4: "gt", 5: "ge"}
        converted = None
        if predicates:
            converted = [(idx, op_map[op], value) for idx, op, value in predicates]
        rows = self._core.scan(db, dataset, columns, converted, limit, offset)
        return rows, {"rows_returned": len(rows)}

    def query_agg_routed(self, db, dataset, field_index, predicates=None):
        op_map = {0: "eq", 1: "ne", 2: "lt", 3: "le", 4: "gt", 5: "ge"}
        converted = None
        if predicates:
            converted = [(idx, op_map[op], value) for idx, op, value in predicates]
        result = self._core.aggregate(db, dataset, field_index, converted)
        return result, {}


def run_sql_dialect_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    dialect: str,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    core = CppApiClient()
    core.create_database(database)
    core.create_dataset(
        database,
        "users",
        {
            "age": "int32",
            "country": "int32",
            "income": "float64",
        },
    )
    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            core.append_batch(
                database,
                "users",
                {
                    "age": batch["age"],
                    "country": batch["country"],
                    "income": batch["income"],
                },
            )
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            core.append_batch(database, "users", build_batch(rng, current))
            remaining -= current
    append_end = time.perf_counter()

    sql = (
        "SELECT SUM(income) AS sum, COUNT(income) AS count, "
        "MIN(income) AS min, MAX(income) AS max "
        "FROM users WHERE age > 30 AND country = 45"
    )
    query = parse_sql(sql, dialect=dialect)
    fields = [("age", "int32"), ("country", "int32"), ("income", "float64")]
    result_rows, _ = execute_query(SqlCppClient(core), database, query, fields)
    result = result_rows[0] if result_rows else {}
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


def run_mysql_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
) -> dict[str, float]:
    rng = random.Random(seed)
    conn = MySQLConnection(host=host, port=port, database=database)
    cur = conn.cursor()
    cur.execute(f"CREATE DATABASE {database}")
    cur.execute(f"USE {database}")
    cur.execute("CREATE TABLE users (age INT, country INT, income DOUBLE)")

    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            batch = build_batch(rng, 1)
            cur.execute(
                "INSERT INTO users (age, country, income) VALUES "
                f"({batch['age'][0]}, {batch['country'][0]}, {_format_value(batch['income'][0])})"
            )
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            batch = build_batch(rng, current)
            values = []
            for age, country, income in zip(
                batch["age"], batch["country"], batch["income"]
            ):
                values.append(f"({age}, {country}, {_format_value(income)})")
            cur.execute(
                "INSERT INTO users (age, country, income) VALUES " + ", ".join(values)
            )
            remaining -= current
    append_end = time.perf_counter()

    cur.execute(
        "SELECT SUM(income), COUNT(income), MIN(income), MAX(income) "
        "FROM users WHERE age > 30 AND country = 45 GROUP BY country"
    )
    result = cur.fetchall()
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
    }


def _format_value(value) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, str):
        return "'" + value.replace("'", "''") + "'"
    return str(value)


def print_results(label: str, rows: int, stats: dict[str, float]) -> None:
    print(f"benchmark={label}")
    print(f"rows={rows}")
    print(f"append_seconds={stats['append_seconds']:.6f}")
    print(f"append_rows_per_sec={stats['append_rows_per_sec']:.2f}")
    print(f"query_seconds={stats['query_seconds']:.6f}")
    print(f"result={stats['result']}")


def main() -> None:
    parser = argparse.ArgumentParser(description="MimicDB/MimicAPI/Mongo benchmark suite")
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch-size", type=int, default=10_000)
    parser.add_argument("--append-mode", choices=["batch", "row"], default="batch")
    parser.add_argument("--backend", choices=["cpp", "py"], default="cpp")
    parser.add_argument("--transport", choices=["local", "network"], default="local")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--server-bin", default=None)
    parser.add_argument("--database", default="bench")
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--include-cpp-core", action="store_true")
    args = parser.parse_args()

    server_proc = None
    if args.transport == "network":
        repo_root = Path(__file__).resolve().parents[1]
        if str(repo_root) not in sys.path:
            sys.path.insert(0, str(repo_root))
        if args.server_bin is not None:
            resolved = resolve_server_bin(args.server_bin, repo_root)
            server_proc = subprocess.Popen([resolved, str(args.port)])
            time.sleep(0.2)

    try:
        host = args.host if args.transport == "network" else None
        port = args.port if args.transport == "network" else None
        run_id = args.run_id
        if run_id is None and args.transport == "network":
            run_id = str(int(time.time()))
        suffix = f"_{run_id}" if run_id else ""
        dataset_db = f"{args.database}_dataset{suffix}"
        api_db = f"{args.database}_api{suffix}"
        mongo_db = f"{args.database}_mongo{suffix}"
        mysql_db = f"{args.database}_mysql{suffix}"
        cpp_core_db = f"{args.database}_cppcore{suffix}"

        dataset_stats = run_dataset_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.append_mode,
            args.backend,
            host,
            port,
            dataset_db,
        )
        print_results("dataset", args.rows, dataset_stats)
        print("---")

        api_stats = run_mimicapi_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.append_mode,
            host,
            port,
            api_db,
        )
        print_results("mimicapi", args.rows, api_stats)
        print("---")

        mongo_stats = run_mongo_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.append_mode,
            host,
            port,
            mongo_db,
        )
        print_results("mongodb_cpp", args.rows, mongo_stats)
        print("---")

        mysql_stats = run_mysql_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.append_mode,
            host,
            port,
            mysql_db,
        )
        print_results("mysql_mariadb_api", args.rows, mysql_stats)
        print("---")
        sql_dialects = ["ansi", "postgres", "mysql", "sqlite", "oracle", "duckdb", "sqlserver"]
        for dialect in sql_dialects:
            sql_db = f"{args.database}_{dialect}{suffix}"
            sql_stats = run_sql_dialect_bench(
                args.rows,
                args.seed,
                args.batch_size,
                args.append_mode,
                dialect,
                sql_db,
            )
            print_results(f"sql_{dialect}", args.rows, sql_stats)
            print("---")
        if args.include_cpp_core:
            print("---")
            cpp_core_stats = run_cpp_core_bench(
                args.rows,
                args.seed,
                args.batch_size,
                args.append_mode,
                cpp_core_db,
            )
            print_results("cpp_core", args.rows, cpp_core_stats)
    finally:
        if server_proc is not None:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()


if __name__ == "__main__":
    main()
