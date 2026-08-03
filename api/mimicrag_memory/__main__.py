from __future__ import annotations

import argparse
import json

from .models import MemoryNamespace, MemoryRecord, Visibility
from .store import MemoryStore


def main() -> int:
    parser = argparse.ArgumentParser(description="MimicRAG explicit memory API")
    parser.add_argument("--store", default="mimicrag-memory.db"); parser.add_argument("--tenant", required=True); parser.add_argument("--owner", required=True)
    sub = parser.add_subparsers(dest="operation", required=True)
    event = sub.add_parser("event"); event.add_argument("kind", choices=("conversation", "tool_result", "correction", "task_outcome", "observation")); event.add_argument("content")
    remember = sub.add_parser("remember"); remember.add_argument("namespace", choices=[item.value for item in MemoryNamespace]); remember.add_argument("subject"); remember.add_argument("statement"); remember.add_argument("--evidence", action="append", required=True); remember.add_argument("--sensitivity", default="internal"); remember.add_argument("--purpose", action="append", default=[]); remember.add_argument("--confirm", action="store_true")
    recall = sub.add_parser("recall"); recall.add_argument("query"); recall.add_argument("--purpose", default="conversation"); recall.add_argument("--limit", type=int, default=12)
    correct = sub.add_parser("correct"); correct.add_argument("memory_id"); correct.add_argument("statement"); correct.add_argument("evidence_id")
    forget = sub.add_parser("forget"); forget.add_argument("memory_id"); forget.add_argument("--erase-evidence", action="store_true")
    inspect = sub.add_parser("inspect"); inspect.add_argument("memory_id")
    export = sub.add_parser("export"); export.add_argument("--include-evidence", action="store_true")
    args = parser.parse_args(); store = MemoryStore(args.store)
    try:
        if args.operation == "event": result = {"evidence_id": store.append_evidence(tenant=args.tenant, owner=args.owner, kind=args.kind, content=args.content, provenance="explicit_cli")}
        elif args.operation == "remember":
            record = MemoryRecord(args.tenant, args.owner, MemoryNamespace(args.namespace), args.subject, args.statement, Visibility.PRIVATE,
                args.sensitivity, args.purpose or ["conversation"], args.evidence)
            result = store.remember(record, confirmed=args.confirm)
        elif args.operation == "recall": result = store.recall(args.query, tenant=args.tenant, owner=args.owner, purpose=args.purpose, limit=args.limit)
        elif args.operation == "correct": result = store.correct(args.memory_id, statement=args.statement, correction_evidence_id=args.evidence_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "forget": result = store.forget(args.memory_id, tenant=args.tenant, owner=args.owner, erase_evidence=args.erase_evidence)
        elif args.operation == "inspect": result = store.inspect(args.memory_id, tenant=args.tenant, owner=args.owner)
        else: result = store.export(tenant=args.tenant, owner=args.owner, include_evidence_content=args.include_evidence)
        print(json.dumps(result, indent=2, ensure_ascii=False)); return 0
    finally: store.close()


if __name__ == "__main__": raise SystemExit(main())
