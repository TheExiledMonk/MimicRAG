import argparse
import random
import time

from pcdb import Dataset


_LOCK_MODES = ("append-only", "update-only", "full-crud")


def run_bench(
    rows: int,
    seed: int,
    batch_size: int,
    backend: str,
    append_mode: str,
    debug: bool,
    lock_mode: str,
) -> None:
    rng = random.Random(seed)
    users = Dataset(
        name="users",
        fields={
            "age": "int32",
            "country": "int32",
            "income": "float64",
        },
        use_cpp=backend == "cpp",
        lock_mode=lock_mode,
    )

    start = time.perf_counter()
    append_error = None
    if append_mode == "row":
        try:
            for _ in range(rows):
                age = rng.randint(18, 90)
                country = rng.randint(1, 200)
                income = float(rng.randint(10_000, 150_000))
                if rng.random() < 0.05:
                    income = None
                users.append(age=age, country=country, income=income)
        except RuntimeError as exc:
            append_error = exc
    else:
        remaining = rows
        try:
            while remaining > 0:
                current = batch_size if remaining > batch_size else remaining
                ages = []
                countries = []
                incomes = []
                for _ in range(current):
                    ages.append(rng.randint(18, 90))
                    countries.append(rng.randint(1, 200))
                    income = float(rng.randint(10_000, 150_000))
                    if rng.random() < 0.05:
                        income = None
                    incomes.append(income)
                users.append_batch(
                    {
                        "age": ages,
                        "country": countries,
                        "income": incomes,
                    }
                )
                remaining -= current
        except RuntimeError as exc:
            append_error = exc
    append_end = time.perf_counter()

    result = None
    stats = None
    if append_error is None:
        query = users.filter(age_gt=30, country_eq=45)
        if debug:
            result, stats = query.aggregate(
                sum="income",
                count=True,
                min="income",
                max="income",
                debug=True,
            )
        else:
            result = query.aggregate(sum="income", count=True, min="income", max="income")
    query_end = time.perf_counter()

    append_seconds = append_end - start
    query_seconds = query_end - append_end

    print(f"backend={backend}")
    print(f"lock_mode={lock_mode}")
    print(f"append_mode={append_mode}")
    print(f"rows={rows}")
    print(f"append_seconds={append_seconds:.6f}")
    if append_error is None:
        print(f"append_rows_per_sec={rows / append_seconds:.2f}")
        print(f"query_seconds={query_seconds:.6f}")
        print(f"result={result}")
        if debug and stats is not None:
            print(f"stats={stats}")
    else:
        print(f"append_error={append_error}")


def main() -> None:
    parser = argparse.ArgumentParser(description="PCDB Python API benchmark")
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch-size", type=int, default=10_000)
    parser.add_argument("--backend", choices=["cpp", "py"], default="cpp")
    parser.add_argument("--append-mode", choices=["batch", "row"], default="batch")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--lock-mode", choices=_LOCK_MODES, default="append-only")
    parser.add_argument("--all-lock-modes", action="store_true")
    args = parser.parse_args()
    modes = _LOCK_MODES if args.all_lock_modes else (args.lock_mode,)
    for lock_mode in modes:
        run_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.backend,
            args.append_mode,
            args.debug,
            lock_mode,
        )
        if args.all_lock_modes:
            print("---")


if __name__ == "__main__":
    main()
