import argparse
import os
import random
import shutil
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


def _perf_available() -> bool:
    return shutil.which("perf") is not None


def _perf_events_arg(events: str | None) -> str:
    return events or "branches,branch-misses,cache-misses,cache-references"


def _print_perf_stats(stderr: str) -> None:
    for line in stderr.splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 3:
            continue
        value, unit, event = parts[0], parts[1], parts[2]
        if not value or value == "<not supported>":
            continue
        event_key = event.replace("/", "_").replace(" ", "_")
        if unit:
            print(f"perf_{event_key}={value} {unit}")
        else:
            print(f"perf_{event_key}={value}")


def _run_with_perf(cmd: list[str], cwd: Path, env: dict[str, str], events: str | None) -> None:
    if not _perf_available():
        print("perf_warning=perf_not_found")
        subprocess.run(cmd, check=True, cwd=cwd, env=env)
        return
    perf_cmd = ["perf", "stat", "-x", ",", "-e", _perf_events_arg(events), "--"] + cmd
    result = subprocess.run(perf_cmd, check=True, cwd=cwd, env=env, stderr=subprocess.PIPE)
    _print_perf_stats(result.stderr.decode("utf-8", errors="replace"))


def _mongo_cpp_available() -> bool:
    try:
        from mimicapi import mongodb_cpp as _mongodb_cpp_mod
    except Exception as exc:
        print(f"mongodb_cpp_skip_reason=import_error:{exc}")
        return False
    if getattr(_mongodb_cpp_mod, "_mimicapi_mongo", None) is None:
        err = getattr(_mongodb_cpp_mod, "_mimicapi_mongo_error", None)
        if err:
            print(f"mongodb_cpp_skip_reason=core_module_missing:{err}")
        else:
            print("mongodb_cpp_skip_reason=core_module_missing")
        return False
    return True


def _cpp_core_available() -> bool:
    try:
        from mimicapi import _mimicapi_core as _core_mod
    except Exception as exc:
        try:
            from mimicapi import cpp_api as _cpp_api_mod
        except Exception as inner_exc:
            print(f"sql_cpp_skip_reason=core_import_failed:{inner_exc}")
            return False
        if getattr(_cpp_api_mod, "_mimicapi_core", None) is None:
            err = getattr(_cpp_api_mod, "_mimicapi_core_error", None)
            if err:
                print(f"sql_cpp_skip_reason=core_module_missing:{err}")
            else:
                print("sql_cpp_skip_reason=core_module_missing")
            return False
        return True
    if _core_mod is None:
        print("sql_cpp_skip_reason=core_module_none")
        return False
    return _core_mod is not None


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


def _extract_count(result) -> int:
    if isinstance(result, dict):
        value = result.get("count")
        return int(value) if value is not None else 0
    if isinstance(result, list):
        if not result:
            return 0
        first = result[0]
        if isinstance(first, dict):
            value = first.get("count")
            return int(value) if value is not None else 0
        if isinstance(first, (list, tuple)) and len(first) >= 2:
            return int(first[1])
    return 0


def _extract_rows_scanned(result, fallback: int) -> int:
    if isinstance(result, dict) and "rows_scanned" in result:
        return int(result["rows_scanned"])
    return fallback


def _selectivity(count: int, rows: int) -> float:
    if rows <= 0:
        return 0.0
    return count / rows


def _run_mask_reuse_dataset(users: Dataset) -> dict[str, float]:
    start = time.perf_counter()
    single_result = users.filter(age_gt=30, country_eq=45).aggregate(sum="income")
    single_end = time.perf_counter()
    multi_result = users.filter(age_gt=30, country_eq=45).aggregate(
        sum="income", count=True, min="income", max="income"
    )
    multi_end = time.perf_counter()
    return {
        "query_single_seconds": single_end - start,
        "query_multi_seconds": multi_end - single_end,
        "single_result": single_result,
        "multi_result": multi_result,
    }




def _compare_agg_results(left, right) -> bool:
    if not isinstance(left, dict) or not isinstance(right, dict):
        return False
    for key in ("count", "sum", "min", "max"):
        if key in left or key in right:
            if left.get(key) != right.get(key):
                return False
    return True


def _validate_multi_aggregate(stats: dict[str, float]) -> None:
    single = stats.get("single_result")
    multi = stats.get("multi_result")
    if isinstance(single, dict) and isinstance(multi, dict):
        if "sum" in single and "sum" in multi and single["sum"] != multi["sum"]:
            raise RuntimeError("multi-aggregate sum mismatch")




def _maybe_sleep(ms: int) -> None:
    if ms <= 0:
        return
    time.sleep(ms / 1000.0)


def run_dataset_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    backend: str,
    host: str | None,
    port: int | None,
    database: str,
    mask_reuse: bool,
    append_sleep_ms: int,
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
            _maybe_sleep(append_sleep_ms)
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            users.append_batch(build_batch(rng, current))
            remaining -= current
            _maybe_sleep(append_sleep_ms)
    append_end = time.perf_counter()

    result = users.filter(age_gt=30, country_eq=45).aggregate(
        sum="income", count=True, min="income", max="income"
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    count = _extract_count(result)
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        **(_run_mask_reuse_dataset(users) if mask_reuse else {}),
    }


def run_mimicapi_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
    mask_reuse: bool,
    append_sleep_ms: int,
    cleanup: bool,
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
            _maybe_sleep(append_sleep_ms)
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            client.append_batch_fanout(database, "users", fields, build_batch(rng, current))
            remaining -= current
            _maybe_sleep(append_sleep_ms)
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
    count = _extract_count(result)
    reuse_stats = {}
    if mask_reuse:
        single_start = time.perf_counter()
        single_result, _ = client.query_agg_routed(
            database,
            "users",
            field_index=2,
            predicates=[(0, 4, 30.0), (1, 0, 45.0)],
        )
        single_end = time.perf_counter()
        multi_result, _ = client.query_agg_routed(
            database,
            "users",
            field_index=2,
            predicates=[(0, 4, 30.0), (1, 0, 45.0)],
        )
        multi_end = time.perf_counter()
        reuse_stats = {
            "query_single_seconds": single_end - single_start,
            "query_multi_seconds": multi_end - single_end,
            "single_result": single_result,
            "multi_result": multi_result,
        }
    stats = {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        **reuse_stats,
    }
    if cleanup:
        client.drop_database_all(database)
    return stats


def run_mongo_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
    mask_reuse: bool,
    append_sleep_ms: int,
    cleanup: bool,
    cache_compare: bool,
) -> dict[str, float]:
    if not _mongo_cpp_available():
        print("mongodb_cpp_skip=extension_unavailable")
        return {
            "append_seconds": 0.0,
            "append_rows_per_sec": 0.0,
            "query_seconds": 0.0,
            "result": "skipped",
            "rows_scanned": 0.0,
            "selectivity": 0.0,
        }
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
            _maybe_sleep(append_sleep_ms)
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
            _maybe_sleep(append_sleep_ms)
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
    cache_query_seconds = None
    cache_query_stats = None
    if cache_compare:
        cache_start = time.perf_counter()
        _ = collection.aggregate(
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
        cache_query_seconds = time.perf_counter() - cache_start
        cache_query_stats = collection.stats(db, "users")

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    count = _extract_count(result)
    reuse_stats = {}
    if mask_reuse:
        single_start = time.perf_counter()
        single_result = collection.aggregate(
            db,
            "users",
            [
                {"$match": {"age": {"$gt": 30}, "country": 45}},
                {"$group": {"_id": None, "sum": {"$sum": "$income"}}},
            ],
        )
        single_end = time.perf_counter()
        multi_result = collection.aggregate(
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
        multi_end = time.perf_counter()
        reuse_stats = {
            "query_single_seconds": single_end - single_start,
            "query_multi_seconds": multi_end - single_end,
            "single_result": single_result,
            "multi_result": multi_result,
        }
    base_stats = collection.stats(db, "users")
    stats = {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        "mongo_last_scan_rows": base_stats.get("last_scan_rows", 0),
        "mongo_last_returned_rows": base_stats.get("last_returned_rows", 0),
        "mongo_last_full_rebuild": base_stats.get("last_full_rebuild", False),
        **reuse_stats,
    }
    if cache_query_seconds is not None and cache_query_stats is not None:
        stats["cache_query_seconds"] = cache_query_seconds
        stats["cache_last_scan_rows"] = cache_query_stats.get("last_scan_rows", 0)
        stats["cache_last_returned_rows"] = cache_query_stats.get("last_returned_rows", 0)
        stats["cache_last_full_rebuild"] = cache_query_stats.get("last_full_rebuild", False)
    _ = cleanup
    return stats


def run_cpp_core_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    database: str,
    mask_reuse: bool,
    append_sleep_ms: int,
    cleanup: bool,
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
            _maybe_sleep(append_sleep_ms)
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            client.append_batch(database, "users", build_batch(rng, current))
            remaining -= current
            _maybe_sleep(append_sleep_ms)
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
    count = _extract_count(result)
    reuse_stats = {}
    if mask_reuse:
        single_start = time.perf_counter()
        single_result = client.aggregate(
            database,
            "users",
            field_index=2,
            predicates=[(0, "gt", 30.0), (1, "eq", 45.0)],
        )
        single_end = time.perf_counter()
        multi_result = client.aggregate(
            database,
            "users",
            field_index=2,
            predicates=[(0, "gt", 30.0), (1, "eq", 45.0)],
        )
        multi_end = time.perf_counter()
        reuse_stats = {
            "query_single_seconds": single_end - single_start,
            "query_multi_seconds": multi_end - single_end,
            "single_result": single_result,
            "multi_result": multi_result,
        }
    stats = {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        **reuse_stats,
    }
    if cleanup:
        client.drop_database(database)
    return stats


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
    mask_reuse: bool,
    append_sleep_ms: int,
    cleanup: bool,
) -> dict[str, float]:
    if not _cpp_core_available():
        print("sql_cpp_skip=extension_unavailable")
        return {
            "append_seconds": 0.0,
            "append_rows_per_sec": 0.0,
            "query_seconds": 0.0,
            "result": "skipped",
            "rows_scanned": 0.0,
            "selectivity": 0.0,
        }
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
            _maybe_sleep(append_sleep_ms)
    else:
        remaining = rows
        while remaining > 0:
            current = batch_size if remaining > batch_size else remaining
            core.append_batch(database, "users", build_batch(rng, current))
            remaining -= current
            _maybe_sleep(append_sleep_ms)
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
    count = _extract_count(result)
    reuse_stats = {}
    if mask_reuse:
        single_sql = "SELECT SUM(income) AS sum FROM users WHERE age > 30 AND country = 45"
        single_query = parse_sql(single_sql, dialect=dialect)
        single_start = time.perf_counter()
        single_rows, _ = execute_query(SqlCppClient(core), database, single_query, fields)
        single_end = time.perf_counter()
        multi_rows, _ = execute_query(SqlCppClient(core), database, query, fields)
        multi_end = time.perf_counter()
        reuse_stats = {
            "query_single_seconds": single_end - single_start,
            "query_multi_seconds": multi_end - single_end,
            "single_result": single_rows[0] if single_rows else {},
            "multi_result": multi_rows[0] if multi_rows else {},
        }
    stats = {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        **reuse_stats,
    }
    if cleanup:
        core.drop_database(database)
    return stats


def run_mysql_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str | None,
    port: int | None,
    database: str,
    mask_reuse: bool,
    append_sleep_ms: int,
    cleanup: bool,
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
            _maybe_sleep(append_sleep_ms)
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
            _maybe_sleep(append_sleep_ms)
    append_end = time.perf_counter()

    cur.execute(
        "SELECT SUM(income), COUNT(income), MIN(income), MAX(income) "
        "FROM users WHERE age > 30 AND country = 45 GROUP BY country"
    )
    result = cur.fetchall()
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    count = _extract_count(result)
    reuse_stats = {}
    if mask_reuse:
        single_start = time.perf_counter()
        cur.execute(
            "SELECT SUM(income) FROM users WHERE age > 30 AND country = 45"
        )
        single_result = cur.fetchall()
        single_end = time.perf_counter()
        cur.execute(
            "SELECT SUM(income), COUNT(income), MIN(income), MAX(income) "
            "FROM users WHERE age > 30 AND country = 45 GROUP BY country"
        )
        multi_result = cur.fetchall()
        multi_end = time.perf_counter()
        reuse_stats = {
            "query_single_seconds": single_end - single_start,
            "query_multi_seconds": multi_end - single_end,
            "single_result": single_result,
            "multi_result": multi_result,
        }
    stats = {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
        "result": result,
        "rows_scanned": _extract_rows_scanned(result, rows),
        "selectivity": _selectivity(count, rows),
        **reuse_stats,
    }
    _ = cleanup
    return stats


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
    if "rows_scanned" in stats:
        print(f"rows_scanned={stats['rows_scanned']}")
    if "selectivity" in stats:
        print(f"selectivity={stats['selectivity']:.6f}")
    if "query_single_seconds" in stats:
        print(f"query_single_seconds={stats['query_single_seconds']:.6f}")
    if "query_multi_seconds" in stats:
        print(f"query_multi_seconds={stats['query_multi_seconds']:.6f}")
    if "mongo_last_scan_rows" in stats:
        print(f"mongo_last_scan_rows={stats['mongo_last_scan_rows']}")
    if "mongo_last_returned_rows" in stats:
        print(f"mongo_last_returned_rows={stats['mongo_last_returned_rows']}")
    if "mongo_last_full_rebuild" in stats:
        print(f"mongo_last_full_rebuild={stats['mongo_last_full_rebuild']}")
    if "cache_query_seconds" in stats:
        print(f"cache_query_seconds={stats['cache_query_seconds']:.6f}")
    if "cache_last_scan_rows" in stats:
        print(f"cache_last_scan_rows={stats['cache_last_scan_rows']}")
    if "cache_last_returned_rows" in stats:
        print(f"cache_last_returned_rows={stats['cache_last_returned_rows']}")
    if "cache_last_full_rebuild" in stats:
        print(f"cache_last_full_rebuild={stats['cache_last_full_rebuild']}")
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
    parser.add_argument("--sweep", action="store_true")
    parser.add_argument("--mask-reuse", action="store_true")
    parser.add_argument("--sweep-max-rows", type=int, default=5_000_000)
    parser.add_argument("--append-sleep-ms", type=int, default=0)
    parser.add_argument("--no-cleanup", action="store_false", dest="cleanup", default=True)
    parser.add_argument("--backends", default="all")
    parser.add_argument("--no-fork-per-backend", action="store_false",
                        dest="fork_per_backend", default=True)
    parser.add_argument("--only-backend", default=None)
    parser.add_argument("--dialect", default=None)
    parser.add_argument("--no-server", action="store_true")
    parser.add_argument("--perf-counters", action="store_true")
    parser.add_argument("--perf-events", default=None)
    parser.add_argument("--_perf-child", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--mongo-cache-compare", action="store_true")
    args = parser.parse_args()

    if args.perf_counters and not args._perf_child and not args.fork_per_backend:
        script = Path(__file__).resolve()
        perf_args = [str(script)] + [
            arg for arg in sys.argv[1:] if arg != "--perf-counters"
        ]
        perf_args.append("--_perf-child")
        _run_with_perf([sys.executable] + perf_args, script.parents[1],
                       os.environ.copy(), args.perf_events)
        return

    server_proc = None
    if args.transport == "network" and not args.no_server:
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

        row_sweep = [args.rows]
        if args.sweep:
            sweep_rows = [200_000, 1_000_000, 5_000_000, 50_000_000]
            if args.sweep_max_rows > 0:
                sweep_rows = [r for r in sweep_rows if r <= args.sweep_max_rows]
            if not sweep_rows:
                sweep_rows = [args.rows]
            row_sweep = sweep_rows

        backends = [b.strip() for b in args.backends.split(",")] if args.backends else ["all"]
        if "all" in backends:
            backends = ["dataset", "mimicapi", "mongodb_cpp", "mysql", "sql"]
            if args.include_cpp_core:
                backends.append("cpp_core")

        if args.only_backend:
            backends = [args.only_backend]

        if "mongodb_cpp" in backends and not _mongo_cpp_available():
            print("mongodb_cpp_skip=extension_unavailable")
            backends = [b for b in backends if b != "mongodb_cpp"]

        if "sql" in backends and not _cpp_core_available():
            print("sql_cpp_skip=extension_unavailable")
            backends = [b for b in backends if b != "sql"]

        if "cpp_core" in backends and not _cpp_core_available():
            print("cpp_core_skip=extension_unavailable")
            backends = [b for b in backends if b != "cpp_core"]

        if args.fork_per_backend and args.only_backend is None:
            script = Path(__file__).resolve()
            repo_root = script.parents[1]
            child_env = os.environ.copy()
            child_env.setdefault("PYTHONPATH", "api")
            for rows in row_sweep:
                for backend in backends:
                    if backend == "sql":
                        dialects = [args.dialect] if args.dialect else [
                            "ansi", "postgres", "mysql", "sqlite", "oracle", "duckdb", "sqlserver"
                        ]
                        for dialect in dialects:
                            child_args = [
                                str(script),
                                "--rows", str(rows),
                                "--seed", str(args.seed),
                                "--batch-size", str(args.batch_size),
                                "--append-mode", args.append_mode,
                                "--backend", args.backend,
                                "--transport", args.transport,
                                "--host", args.host,
                                "--port", str(args.port),
                                "--database", args.database,
                                "--only-backend", "sql",
                                "--dialect", dialect,
                                "--append-sleep-ms", str(args.append_sleep_ms),
                            ]
                            if args.server_bin:
                                child_args += ["--server-bin", args.server_bin]
                            if args.mask_reuse:
                                child_args.append("--mask-reuse")
                            if not args.cleanup:
                                child_args.append("--no-cleanup")
                            if args.mongo_cache_compare:
                                child_args.append("--mongo-cache-compare")
                            if args.perf_counters:
                                child_args.append("--_perf-child")
                                child_args.extend(["--perf-events", args.perf_events or ""])
                            if args.transport == "network":
                                child_args.append("--no-server")
                            if args.perf_counters:
                                _run_with_perf([sys.executable] + child_args, repo_root,
                                               child_env, args.perf_events)
                            else:
                                subprocess.run([sys.executable] + child_args, check=True,
                                               cwd=repo_root, env=child_env)
                    else:
                        child_args = [
                            str(script),
                            "--rows", str(rows),
                            "--seed", str(args.seed),
                            "--batch-size", str(args.batch_size),
                            "--append-mode", args.append_mode,
                            "--backend", args.backend,
                            "--transport", args.transport,
                            "--host", args.host,
                            "--port", str(args.port),
                            "--database", args.database,
                            "--only-backend", backend,
                            "--append-sleep-ms", str(args.append_sleep_ms),
                        ]
                        if args.server_bin:
                            child_args += ["--server-bin", args.server_bin]
                        if args.mask_reuse:
                            child_args.append("--mask-reuse")
                        if not args.cleanup:
                            child_args.append("--no-cleanup")
                        if args.mongo_cache_compare:
                            child_args.append("--mongo-cache-compare")
                        if args.perf_counters:
                            child_args.append("--_perf-child")
                            child_args.extend(["--perf-events", args.perf_events or ""])
                        if args.transport == "network":
                            child_args.append("--no-server")
                        if args.perf_counters:
                            _run_with_perf([sys.executable] + child_args, repo_root,
                                           child_env, args.perf_events)
                        else:
                            subprocess.run([sys.executable] + child_args, check=True,
                                           cwd=repo_root, env=child_env)
            return

        for rows in row_sweep:
            print(f"starting sweep rows={rows}", flush=True)
            sweep_suffix = f"{suffix}_{rows}" if suffix else f"_{rows}"
            if "dataset" in backends:
                dataset_stats = run_dataset_bench(
                    rows,
                    args.seed,
                    args.batch_size,
                    args.append_mode,
                    args.backend,
                    host,
                    port,
                    f"{dataset_db}{sweep_suffix}",
                    args.mask_reuse,
                    args.append_sleep_ms,
                )
                print_results("dataset", rows, dataset_stats)
                print("---")

            if "mimicapi" in backends:
                api_stats = run_mimicapi_bench(
                    rows,
                    args.seed,
                    args.batch_size,
                    args.append_mode,
                    host,
                    port,
                    f"{api_db}{sweep_suffix}",
                    args.mask_reuse,
                    args.append_sleep_ms,
                    args.cleanup,
                )
                print_results("mimicapi", rows, api_stats)
                print("---")

            if "mongodb_cpp" in backends:
                mongo_stats = run_mongo_bench(
                    rows,
                    args.seed,
                    args.batch_size,
                    args.append_mode,
                    host,
                    port,
                    f"{mongo_db}{sweep_suffix}",
                    args.mask_reuse,
                    args.append_sleep_ms,
                    args.cleanup,
                    args.mongo_cache_compare,
                )
                print_results("mongodb_cpp", rows, mongo_stats)
                print("---")

            if "mysql" in backends:
                mysql_stats = run_mysql_bench(
                    rows,
                    args.seed,
                    args.batch_size,
                    args.append_mode,
                    host,
                    port,
                    f"{mysql_db}{sweep_suffix}",
                    args.mask_reuse,
                    args.append_sleep_ms,
                    args.cleanup,
                )
                print_results("mysql_mariadb_api", rows, mysql_stats)
                print("---")

            if "sql" in backends:
                sql_dialects = [args.dialect] if args.dialect else [
                    "ansi", "postgres", "mysql", "sqlite", "oracle", "duckdb", "sqlserver"
                ]
                for dialect in sql_dialects:
                    sql_db = f"{args.database}_{dialect}{sweep_suffix}"
                    sql_stats = run_sql_dialect_bench(
                        rows,
                        args.seed,
                        args.batch_size,
                        args.append_mode,
                        dialect,
                        sql_db,
                        args.mask_reuse,
                        args.append_sleep_ms,
                        args.cleanup,
                    )
                    print_results(f"sql_{dialect}", rows, sql_stats)
                    print("---")

            if "cpp_core" in backends:
                cpp_core_stats = run_cpp_core_bench(
                    rows,
                    args.seed,
                    args.batch_size,
                    args.append_mode,
                    f"{cpp_core_db}{sweep_suffix}",
                    args.mask_reuse,
                    args.append_sleep_ms,
                    args.cleanup,
                )
                print_results("cpp_core", rows, cpp_core_stats)
                print("---")
    finally:
        if server_proc is not None:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()


if __name__ == "__main__":
    main()
