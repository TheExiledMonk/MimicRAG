#!/usr/bin/env python3
from __future__ import annotations

import argparse
import random
import time

from mimicapi.cpp_api import CppApiClient
from mimicapi.dataset import Dataset


def build_columns(rng: random.Random, count: int) -> dict[str, list]:
    ages = []
    countries = []
    incomes = []
    for _ in range(count):
        ages.append(rng.randint(18, 90))
        countries.append(rng.randint(1, 200))
        incomes.append(float(rng.randint(10_000, 150_000)))
    return {"age": ages, "country": countries, "income": incomes}


def bench_cpp(rows: int, batch_size: int, seed: int) -> dict:
    rng = random.Random(seed)
    client = CppApiClient()
    client.create_database("bench")
    client.create_dataset(
        "bench",
        "users",
        {"age": "int32", "country": "int32", "income": "float64"},
    )

    start = time.perf_counter()
    remaining = rows
    while remaining > 0:
        batch = min(batch_size, remaining)
        columns = build_columns(rng, batch)
        client.append_batch("bench", "users", columns)
        remaining -= batch
    append_end = time.perf_counter()

    predicates = [
        (0, "gt", 30.0),
        (1, "eq", 45.0),
    ]
    requests = [
        {"kind": "SUM", "field_index": 2, "alias": "sum"},
        {"kind": "MIN", "field_index": 2, "alias": "min"},
        {"kind": "MAX", "field_index": 2, "alias": "max"},
        {"kind": "COUNT", "field_index": 2, "alias": "count"},
    ]
    result = client.aggregate_multi("bench", "users", requests, predicates)
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds if append_seconds > 0 else 0.0,
        "query_seconds": query_seconds,
        "result": result,
    }


def bench_python(rows: int, batch_size: int, seed: int) -> dict:
    rng = random.Random(seed)
    users = Dataset(
        name="users",
        fields={"age": "int32", "country": "int32", "income": "float64"},
        use_cpp=False,
    )

    start = time.perf_counter()
    remaining = rows
    while remaining > 0:
        batch = min(batch_size, remaining)
        columns = build_columns(rng, batch)
        users.append_batch(columns)
        remaining -= batch
    append_end = time.perf_counter()

    result = (
        users.filter(age_gt=30, country_eq=45)
        .aggregate(sum="income", min="income", max="income", count=True)
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds if append_seconds > 0 else 0.0,
        "query_seconds": query_seconds,
        "result": result,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="C++ API vs Python baseline benchmark")
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--batch-size", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    cpp_stats = bench_cpp(args.rows, args.batch_size, args.seed)
    print("benchmark=cpp_api")
    for key, value in cpp_stats.items():
        print(f"{key}={value}")
    print("---")

    py_stats = bench_python(args.rows, args.batch_size, args.seed)
    print("benchmark=python_baseline")
    for key, value in py_stats.items():
        print(f"{key}={value}")


if __name__ == "__main__":
    main()
