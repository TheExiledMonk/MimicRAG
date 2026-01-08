import argparse
import random
import time

from mimicapi import CppApiClient


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


def main() -> None:
    parser = argparse.ArgumentParser(description="MimicAPI C++ core benchmark")
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch-size", type=int, default=10_000)
    parser.add_argument("--append-mode", choices=["batch", "row"], default="batch")
    parser.add_argument("--database", default="bench_cpp")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    client = CppApiClient()
    client.create_database(args.database)
    client.create_dataset(
        args.database,
        "users",
        {
            "age": "int32",
            "country": "int32",
            "income": "float64",
        },
    )
    start = time.perf_counter()
    if args.append_mode == "row":
        for _ in range(args.rows):
            batch = build_batch(rng, 1)
            client.append_batch(args.database, "users", batch)
    else:
        remaining = args.rows
        while remaining > 0:
            current = args.batch_size if remaining > args.batch_size else remaining
            client.append_batch(args.database, "users", build_batch(rng, current))
            remaining -= current
    append_end = time.perf_counter()

    result = client.aggregate(
        args.database,
        "users",
        field_index=2,
        predicates=[(0, "gt", 30.0), (1, "eq", 45.0)],
    )
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end

    print(f"rows={args.rows}")
    print(f"append_seconds={append_seconds:.6f}")
    print(f"append_rows_per_sec={args.rows / append_seconds:.2f}")
    print(f"query_seconds={query_seconds:.6f}")
    print(f"result={result}")


if __name__ == "__main__":
    main()
