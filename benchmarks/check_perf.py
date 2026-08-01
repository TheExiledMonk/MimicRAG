import argparse
import json
import subprocess
import sys


def parse_kv_line(line: str) -> dict[str, str]:
    parts = line.strip().split()
    result: dict[str, str] = {}
    for part in parts:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        result[key] = value
    return result


def load_thresholds(path: str) -> dict[str, float]:
    with open(path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    return data.get("bench_scan", {})


def run_bench(path: str, rows: int) -> dict[str, str]:
    args = [path, f"--rows={rows}"]
    proc = subprocess.run(args, check=True, capture_output=True, text=True)
    line = proc.stdout.strip().splitlines()[-1]
    return parse_kv_line(line)


def check_thresholds(metrics: dict[str, str], thresholds: dict[str, float]) -> int:
    rows_per_sec = float(metrics.get("rows_per_sec", "0"))
    bytes_per_sec = float(metrics.get("bytes_per_sec", "0"))
    rows_min = float(thresholds.get("rows_per_sec_min", 0))
    bytes_min = float(thresholds.get("bytes_per_sec_min", 0))
    failures: list[str] = []
    if rows_min and rows_per_sec < rows_min:
        failures.append(f"rows_per_sec {rows_per_sec:.2f} < {rows_min:.2f}")
    if bytes_min and bytes_per_sec < bytes_min:
        failures.append(f"bytes_per_sec {bytes_per_sec:.2f} < {bytes_min:.2f}")
    if failures:
        print("perf_regression=" + "; ".join(failures))
        return 1
    print("perf_ok=1")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Check MimicDB perf thresholds")
    parser.add_argument("--bench", default="build/mimicdb_bench_scan")
    parser.add_argument("--rows", type=int, default=1 << 20)
    parser.add_argument("--thresholds", default="benchmarks/perf_thresholds.json")
    args = parser.parse_args()
    thresholds = load_thresholds(args.thresholds)
    metrics = run_bench(args.bench, args.rows)
    return check_thresholds(metrics, thresholds)


if __name__ == "__main__":
    raise SystemExit(main())
