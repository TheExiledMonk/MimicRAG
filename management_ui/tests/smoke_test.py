from __future__ import annotations

import argparse
import json
import sys
from urllib import request


def http_json(method: str, url: str, payload: dict | None = None):
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = request.Request(url, data=data, headers=headers, method=method)
    with request.urlopen(req, timeout=5) as response:
        body = response.read().decode("utf-8")
        return response.status, json.loads(body)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", default="http://127.0.0.1:8000")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--database", default="default")
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--field", required=True)
    args = parser.parse_args()

    status, data = http_json("GET", f"{args.service}/api/health")
    if status != 200 or not data.get("ok"):
        print("health check failed")
        return 1

    status, _ = http_json(
        "POST",
        f"{args.service}/api/connect",
        {"host": args.host, "port": args.port, "database": args.database},
    )
    if status != 200:
        print("connect failed")
        return 1

    status, data = http_json("GET", f"{args.service}/api/databases")
    if status != 200 or args.database not in data.get("databases", []):
        print("database list failed")
        return 1

    status, data = http_json(
        "GET", f"{args.service}/api/datasets?database={args.database}"
    )
    if status != 200 or args.dataset not in data.get("datasets", []):
        print("dataset list failed")
        return 1

    status, data = http_json(
        "GET",
        f"{args.service}/api/schema?database={args.database}&dataset={args.dataset}",
    )
    if status != 200 or not data.get("fields"):
        print("schema fetch failed")
        return 1

    status, _ = http_json(
        "POST",
        f"{args.service}/api/scan",
        {
            "database": args.database,
            "dataset": args.dataset,
            "columns": [args.field],
            "predicates": [],
            "limit": 1,
            "cursor": None,
        },
    )
    if status != 200:
        print("scan failed")
        return 1

    status, _ = http_json(
        "POST",
        f"{args.service}/api/aggregate",
        {
            "database": args.database,
            "dataset": args.dataset,
            "field": args.field,
            "predicates": [],
        },
    )
    if status != 200:
        print("aggregate failed")
        return 1

    print("smoke test ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
