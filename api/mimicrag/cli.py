from __future__ import annotations

import argparse
from dataclasses import asdict
import json

from .config import load_config
from .evaluation import EvaluationCase, evaluate
from .runtime import RagRuntime


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mimicrag")
    parser.add_argument("--config", default="mimicrag.json")
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve")
    serve.add_argument("--host")
    serve.add_argument("--port", type=int)
    ingest = commands.add_parser("ingest")
    ingest.add_argument("path")
    ingest.add_argument("--source-uri")
    ingest.add_argument("--title", default="")
    ingest.add_argument("--tenant", default="default")
    query = commands.add_parser("query")
    query.add_argument("query")
    query.add_argument("--tenant", default="default")
    query.add_argument("--scope", default="public")
    query.add_argument("--top-k", type=int, default=10)
    evaluate_cmd = commands.add_parser("evaluate")
    evaluate_cmd.add_argument("golden_set")
    evaluate_cmd.add_argument("--generate", action="store_true")
    args = parser.parse_args(argv)
    config = load_config(args.config)
    if args.command == "serve":
        try:
            import uvicorn
        except ImportError as exc:
            raise SystemExit("uvicorn is required to serve MimicRAG") from exc
        import os
        os.environ["MIMICRAG_CONFIG"] = args.config
        uvicorn.run("mimicrag.server:app_from_environment", factory=True, host=args.host or config.server.host, port=args.port or config.server.port)
        return 0
    runtime = RagRuntime(config)
    try:
        if args.command == "ingest":
            from pathlib import Path
            path = Path(args.path)
            result, _ = runtime.ingest(text=path.read_text(encoding="utf-8"), source_uri=args.source_uri or path.resolve().as_uri(), tenant_id=args.tenant, title=args.title, background=False)
            print(json.dumps(asdict(result)))
        elif args.command == "query":
            result = runtime.answer(args.query, args.tenant, args.scope, args.top_k)
            print(result.answer)
            print(json.dumps({"trace_id": result.trace_id, "citations": [asdict(item) for item in result.context.citations]}, indent=2))
        elif args.command == "evaluate":
            from pathlib import Path
            raw = json.loads(Path(args.golden_set).read_text(encoding="utf-8"))
            cases = [EvaluationCase(query=item["query"], relevant_source_uris=tuple(item["relevant_source_uris"]), required_answer_terms=tuple(item.get("required_answer_terms", ())), tenant_id=item.get("tenant_id", config.tenant_id), access_scope=item.get("access_scope", "public")) for item in raw]
            print(json.dumps(asdict(evaluate(runtime, cases, generate=args.generate)), indent=2))
        return 0
    finally:
        runtime.close()


if __name__ == "__main__":
    raise SystemExit(main())
