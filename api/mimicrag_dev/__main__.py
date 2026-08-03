from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
from pathlib import Path

from .client import Client


def initialize(path: Path) -> dict:
    path.mkdir(parents=True, exist_ok=True); data = path / "data"; data.mkdir(exist_ok=True)
    threads = max(1, os.cpu_count() or 1)
    config = {"server": {"host": "127.0.0.1", "port": 8080, "data_path": str(data), "worker_threads": threads},
              "ingestion": {"default_mode": "structured"},
              "calibration": {"logical_cpus": threads, "worker_threads": threads, "profile": "balanced"}}
    target = path / "mimicrag.json"
    if not target.exists(): target.write_text(json.dumps(config, indent=2) + "\n")
    return {"initialized": True, "config": str(target), "data": str(data), "calibration": config["calibration"]}


def main() -> int:
    parser = argparse.ArgumentParser(description="MimicRAG developer and inspection CLI")
    parser.add_argument("--server", default=os.getenv("MIMICRAG_BASE_URL", "http://127.0.0.1:8080"))
    parser.add_argument("--api-key", default=os.getenv("MIMICRAG_API_KEY", ""))
    sub = parser.add_subparsers(dest="command", required=True)
    init = sub.add_parser("init"); init.add_argument("directory", nargs="?", default=".")
    inspect = sub.add_parser("inspect"); inspect.add_argument("kind", choices=("storage", "traces", "trace", "corpus", "manifest")); inspect.add_argument("value", nargs="?", default=""); inspect.add_argument("--limit", type=int, default=100)
    export = sub.add_parser("export"); export.add_argument("directory"); export.add_argument("--binary", default="mimicrag_server"); export.add_argument("--config", default="mimicrag.json")
    restore = sub.add_parser("import"); restore.add_argument("snapshot"); restore.add_argument("--binary", default="mimicrag_server"); restore.add_argument("--config", default="mimicrag.json"); restore.add_argument("--destination", default="")
    migrate = sub.add_parser("migrate"); migrate.add_argument("--binary", default="mimicrag_server"); migrate.add_argument("--config", default="mimicrag.json")
    args = parser.parse_args()
    if args.command == "init": result = initialize(Path(args.directory).resolve())
    elif args.command == "inspect":
        if args.kind == "manifest": result = json.loads(Path(args.value or ".mimicrag-ingestion.json").read_text())
        else:
            client = Client(args.server, args.api_key)
            if args.kind == "storage": result = client.storage()
            elif args.kind == "traces": result = client.traces(args.limit)
            elif args.kind == "trace": result = client.trace(args.value)
            else: result = client.retrieve(args.value or "*", top_k=args.limit, retrieval_mode="lexical")
    else:
        native_command = {"export": "snapshot", "import": "restore", "migrate": "migrate"}[args.command]
        command = [args.binary, native_command]
        if args.command == "export": command.append(args.directory)
        elif args.command == "import": command.append(args.snapshot)
        command += ["--config", args.config]
        if args.command == "import" and args.destination: command += ["--destination", args.destination]
        completed = subprocess.run(command, check=True, capture_output=True, text=True); result = json.loads(completed.stdout)
    print(json.dumps(result, indent=2)); return 0


if __name__ == "__main__": raise SystemExit(main())
