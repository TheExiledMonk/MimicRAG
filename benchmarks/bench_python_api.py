import argparse
import random
import subprocess
import time
from pathlib import Path
import sys
import os

from mimicapi import Dataset
from cryptography.hazmat.primitives.asymmetric import ed25519
from cryptography.hazmat.primitives import serialization


_LOCK_MODES = ("append-only", "update-only", "full-crud")


def run_local_bench(
    rows: int,
    seed: int,
    batch_size: int,
    backend: str,
    append_mode: str,
    debug: bool,
    lock_mode: str,
) -> dict[str, float | None]:
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
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": None if append_error is not None else rows / append_seconds,
        "query_seconds": None if append_error is not None else query_seconds,
    }


def run_network_bench(
    rows: int,
    seed: int,
    batch_size: int,
    append_mode: str,
    host: str,
    port: int,
    server_bin: str | None,
    database: str,
    identity_key_path: str | None,
) -> dict[str, float]:
    repo_root = Path(__file__).resolve().parents[1]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    from client.mimicdb_client import MimicDBClient

    rng = random.Random(seed)
    server_proc = None
    if server_bin is not None:
        resolved = resolve_server_bin(server_bin, repo_root)
        server_proc = subprocess.Popen([resolved, str(port)])
        time.sleep(0.2)
        _bootstrap_auth(host, port, identity_key_path)
    try:
        client = MimicDBClient(
            host=host,
            port=port,
            default_db=database,
            identity_key_path=identity_key_path,
        )
        client.ping()
    except Exception:
        if server_proc is not None:
            server_proc.terminate()
            try:
                server_proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                server_proc.kill()
        raise
    fields = [
        ("age", "int32"),
        ("country", "int32"),
        ("income", "float64"),
    ]
    client.create_database(database)
    client.create_dataset("users", fields)

    start = time.perf_counter()
    if append_mode == "row":
        for _ in range(rows):
            age = rng.randint(18, 90)
            country = rng.randint(1, 200)
            income = float(rng.randint(10_000, 150_000))
            if rng.random() < 0.05:
                income = None
            client.append_batch(
                "users",
                fields,
                {
                    "age": [age],
                    "country": [country],
                    "income": [income],
                },
            )
    else:
        remaining = rows
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
            client.append_batch(
                "users",
                fields,
                {
                    "age": ages,
                    "country": countries,
                    "income": incomes,
                },
            )
            remaining -= current
    append_end = time.perf_counter()

    result = client.query_agg("users", field_index=2)
    query_end = time.perf_counter()

    client.close()
    if server_proc is not None:
        server_proc.terminate()
        try:
            server_proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            server_proc.kill()

    append_seconds = append_end - start
    query_seconds = query_end - append_end

    print("transport=network")
    print(f"host={host}")
    print(f"port={port}")
    print(f"database={database}")
    print(f"append_mode={append_mode}")
    print(f"rows={rows}")
    print(f"append_seconds={append_seconds:.6f}")
    print(f"append_rows_per_sec={rows / append_seconds:.2f}")
    print("query=full_scan_agg(field=income)")
    print(f"query_seconds={query_seconds:.6f}")
    print(f"result={result}")
    return {
        "append_seconds": append_seconds,
        "append_rows_per_sec": rows / append_seconds,
        "query_seconds": query_seconds,
    }


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


def _load_or_create_identity_key(path: Path) -> bytes:
    if path.exists():
        data = path.read_bytes()
        key = ed25519.Ed25519PrivateKey.from_private_bytes(data)
    else:
        path.parent.mkdir(parents=True, exist_ok=True)
        key = ed25519.Ed25519PrivateKey.generate()
        path.write_bytes(
            key.private_bytes(
                encoding=serialization.Encoding.Raw,
                format=serialization.PrivateFormat.Raw,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    return key.public_key().public_bytes(
        encoding=serialization.Encoding.Raw,
        format=serialization.PublicFormat.Raw,
    )


def _bootstrap_auth(host: str, port: int, identity_key_path: str | None) -> None:
    if not identity_key_path:
        return
    repo_root = Path(__file__).resolve().parents[1]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    from client.mimicdb_client import MimicDBClient, ProtocolError

    key_path = Path(identity_key_path).expanduser()
    public_key = _load_or_create_identity_key(key_path)
    client = MimicDBClient(
        host=host,
        port=port,
        identity_key_path=str(key_path),
    )
    try:
        client.auth_init_root(public_key, comment="bench-root")
    except ProtocolError:
        pass
    finally:
        client.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="MimicDB Python API benchmark")
    parser.add_argument("--rows", type=int, default=200_000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--batch-size", type=int, default=10_000)
    parser.add_argument("--backend", choices=["cpp", "py"], default="cpp")
    parser.add_argument("--append-mode", choices=["batch", "row"], default="batch")
    parser.add_argument("--transport", choices=["local", "network", "both"], default="local")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--server-bin", default=None)
    parser.add_argument("--database", default="default")
    parser.add_argument("--identity-key", default=None)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--lock-mode", choices=_LOCK_MODES, default="append-only")
    parser.add_argument("--all-lock-modes", action="store_true")
    args = parser.parse_args()
    modes = _LOCK_MODES if args.all_lock_modes else (args.lock_mode,)
    local_results: dict[str, float | None] | None = None
    network_results: dict[str, float] | None = None
    if args.transport in ("local", "both"):
        for lock_mode in modes:
            print("transport=local")
            local_results = run_local_bench(
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
        if args.transport == "both":
            print("===")
    if args.transport in ("network", "both"):
        identity_key_path = args.identity_key or os.getenv("MIMICDB_BENCH_IDENTITY_KEY")
        if not identity_key_path:
            identity_key_path = str(Path.home() / ".mimicdb" / "bench_identity")
        network_results = run_network_bench(
            args.rows,
            args.seed,
            args.batch_size,
            args.append_mode,
            args.host,
            args.port,
            args.server_bin,
            args.database,
            identity_key_path,
        )
    if args.transport == "both" and local_results and network_results:
        print("comparison=local_vs_network")
        print(f"local_append_seconds={local_results['append_seconds']:.6f}")
        if local_results["append_rows_per_sec"] is not None:
            print(f"local_append_rows_per_sec={local_results['append_rows_per_sec']:.2f}")
        if local_results["query_seconds"] is not None:
            print(f"local_query_seconds={local_results['query_seconds']:.6f}")
        print(f"network_append_seconds={network_results['append_seconds']:.6f}")
        print(f"network_append_rows_per_sec={network_results['append_rows_per_sec']:.2f}")
        print(f"network_query_seconds={network_results['query_seconds']:.6f}")


if __name__ == "__main__":
    main()
