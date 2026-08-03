from __future__ import annotations

import argparse
import json
import os

from .models import MemoryNamespace, MemoryRecord, Visibility
from .store import MemoryStore
from .dream import BuiltinWebResearcher, DreamEngine, DreamPolicy, DreamScheduler, JsonSearchResearcher
from .problem_solving import procedure_for_issue
from .providers import AnthropicCompatibleMemoryModel, MiniMaxMemoryModel, OpenAICompatibleMemoryModel


def main() -> int:
    parser = argparse.ArgumentParser(description="MimicRAG explicit memory API")
    parser.add_argument("--store", default="mimicrag-memory.db"); parser.add_argument("--tenant", required=True); parser.add_argument("--owner", required=True)
    sub = parser.add_subparsers(dest="operation", required=True)
    event = sub.add_parser("event"); event.add_argument("kind", choices=("conversation", "tool_result", "correction", "task_outcome", "observation")); event.add_argument("content")
    remember = sub.add_parser("remember"); remember.add_argument("namespace", choices=[item.value for item in MemoryNamespace]); remember.add_argument("subject"); remember.add_argument("statement"); remember.add_argument("--evidence", action="append", required=True); remember.add_argument("--sensitivity", default="internal"); remember.add_argument("--purpose", action="append", default=[]); remember.add_argument("--confirm", action="store_true")
    recall = sub.add_parser("recall"); recall.add_argument("query"); recall.add_argument("--purpose", default="conversation"); recall.add_argument("--limit", type=int, default=12); recall.add_argument("--sensitivity-allowed", action="append", default=[])
    correct = sub.add_parser("correct"); correct.add_argument("memory_id"); correct.add_argument("statement"); correct.add_argument("evidence_id")
    forget = sub.add_parser("forget"); forget.add_argument("memory_id"); forget.add_argument("--erase-evidence", action="store_true")
    inspect = sub.add_parser("inspect"); inspect.add_argument("memory_id")
    confirm = sub.add_parser("confirm"); confirm.add_argument("memory_id")
    reject = sub.add_parser("reject"); reject.add_argument("memory_id"); reject.add_argument("--reason", default="operator rejection")
    review = sub.add_parser("review"); review.add_argument("--status", default="")
    dream = sub.add_parser("dream"); dream.add_argument("--enable", action="store_true", help="explicitly enable this dream cycle"); dream.add_argument("--mode", choices=("light", "deep", "research"), default="light"); dream.add_argument("--research-endpoint", default=os.getenv("MIMICRAG_DREAM_RESEARCH_URL", "")); dream.add_argument("--research-key", default=os.getenv("MIMICRAG_DREAM_RESEARCH_KEY", "")); dream.add_argument("--builtin-search", action="store_true"); dream.add_argument("--authoritative-domain", action="append", default=[]); dream.add_argument("--auto-approve", action="append", default=[]); dream.add_argument("--model-provider", choices=("anthropic", "minimax", "openai-compatible")); dream.add_argument("--model-name", default=""); dream.add_argument("--model-url", default=""); dream.add_argument("--model-key", default="")
    schedule = sub.add_parser("dream-schedule"); schedule.add_argument("--enable", action="store_true"); schedule.add_argument("--interval", type=float, default=3600); schedule.add_argument("--mode", choices=("light", "deep", "research"), default="light"); schedule.add_argument("--builtin-search", action="store_true"); schedule.add_argument("--auto-approve", action="append", default=[])
    dream_report = sub.add_parser("dream-report"); dream_report.add_argument("cycle_id")
    refinements = sub.add_parser("refinements"); refinements.add_argument("--status", default=""); refinements.add_argument("--cycle", default="")
    refinement = sub.add_parser("review-refinement"); refinement.add_argument("refinement_id"); refinement.add_argument("decision", choices=("approved", "rejected")); refinement.add_argument("--reason", default="operator review")
    procedure = sub.add_parser("refined-procedure"); procedure.add_argument("memory_id")
    solve = sub.add_parser("solve-unknown"); solve.add_argument("query"); solve.add_argument("--task-kind", default="issue"); solve.add_argument("--purpose", default="coding"); solve.add_argument("--minimum-score", type=float, default=.6)
    export = sub.add_parser("export"); export.add_argument("--include-evidence", action="store_true")
    args = parser.parse_args(); store = MemoryStore(args.store)
    try:
        if args.operation == "event": result = {"evidence_id": store.append_evidence(tenant=args.tenant, owner=args.owner, kind=args.kind, content=args.content, provenance="explicit_cli")}
        elif args.operation == "remember":
            record = MemoryRecord(args.tenant, args.owner, MemoryNamespace(args.namespace), args.subject, args.statement, Visibility.PRIVATE,
                args.sensitivity, args.purpose or ["conversation"], args.evidence)
            result = store.remember(record, confirmed=args.confirm)
        elif args.operation == "recall": result = store.recall(args.query, tenant=args.tenant, owner=args.owner, purpose=args.purpose, limit=args.limit, sensitivity_allowed=args.sensitivity_allowed or ("public", "internal"))
        elif args.operation == "correct": result = store.correct(args.memory_id, statement=args.statement, correction_evidence_id=args.evidence_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "forget": result = store.forget(args.memory_id, tenant=args.tenant, owner=args.owner, erase_evidence=args.erase_evidence)
        elif args.operation == "inspect": result = store.inspect(args.memory_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "confirm": result = store.confirm(args.memory_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "reject": result = store.reject(args.memory_id, tenant=args.tenant, owner=args.owner, reason=args.reason)
        elif args.operation == "review": result = store.review(tenant=args.tenant, owner=args.owner, status=args.status)
        elif args.operation == "dream":
            researcher = BuiltinWebResearcher() if args.builtin_search else (JsonSearchResearcher(args.research_endpoint, args.research_key) if args.research_endpoint else None)
            key = args.model_key or ({"anthropic": os.getenv("ANTHROPIC_API_KEY", ""), "minimax": os.getenv("MINIMAX_API_KEY", ""), "openai-compatible": os.getenv("OPENAI_API_KEY", "")}.get(args.model_provider, ""))
            model = None
            if args.model_provider == "anthropic": model = AnthropicCompatibleMemoryModel(args.model_name or os.getenv("ANTHROPIC_MODEL", "claude-haiku-4-5"), key, base_url=args.model_url or "https://api.anthropic.com/v1")
            elif args.model_provider == "minimax": model = MiniMaxMemoryModel(args.model_name or os.getenv("MINIMAX_MODEL", "MiniMax-M2.7"), key, base_url=args.model_url or "https://api.minimax.io/anthropic/v1")
            elif args.model_provider == "openai-compatible": model = OpenAICompatibleMemoryModel(args.model_url or "http://127.0.0.1:11434/v1", args.model_name or "local", key)
            policy = DreamPolicy(enabled=args.enable, research_enabled=args.mode == "research" and bool(researcher), authoritative_domains=args.authoritative_domain, auto_approve_operations=set(args.auto_approve), model_enabled=bool(model))
            result = DreamEngine(store, policy=policy, researcher=researcher, model=model).run(tenant=args.tenant, owner=args.owner, mode=args.mode)
        elif args.operation == "dream-schedule":
            researcher = BuiltinWebResearcher() if args.builtin_search else None
            policy = DreamPolicy(enabled=args.enable, research_enabled=args.mode == "research" and bool(researcher), auto_approve_operations=set(args.auto_approve))
            scheduler = DreamScheduler(DreamEngine(store, policy=policy, researcher=researcher), tenant=args.tenant, owner=args.owner, interval_seconds=args.interval, mode=args.mode)
            scheduler.start()
            try:
                while True: scheduler.stop_event.wait(3600)
            except KeyboardInterrupt: scheduler.stop()
            result = {"stopped": True, "last_report": scheduler.last_report, "last_error": scheduler.last_error}
        elif args.operation == "dream-report": result = store.dream_report(args.cycle_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "refinements": result = store.refinements(tenant=args.tenant, owner=args.owner, status=args.status, cycle_id=args.cycle)
        elif args.operation == "review-refinement": result = store.review_refinement(args.refinement_id, tenant=args.tenant, owner=args.owner, decision=args.decision, reason=args.reason)
        elif args.operation == "refined-procedure": result = store.refined_procedure(args.memory_id, tenant=args.tenant, owner=args.owner)
        elif args.operation == "solve-unknown": result = procedure_for_issue(store, args.query, tenant=args.tenant, owner=args.owner, task_kind=args.task_kind, purpose=args.purpose, minimum_score=args.minimum_score)
        else: result = store.export(tenant=args.tenant, owner=args.owner, include_evidence_content=args.include_evidence)
        print(json.dumps(result, indent=2, ensure_ascii=False)); return 0
    finally: store.close()


if __name__ == "__main__": raise SystemExit(main())
