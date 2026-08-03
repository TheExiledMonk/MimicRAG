from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any


@dataclass
class MemoryEvaluation:
    useful_recall: int = 0
    missed_recall: int = 0
    intrusive_recall: int = 0
    stale_recall: int = 0
    harmful_recall: int = 0
    cross_tenant_leakage: int = 0
    deletion_failures: int = 0
    latency_ms: list[float] = field(default_factory=list)
    token_overhead: int = 0
    remote_cost: float = 0.0

    def report(self) -> dict[str, Any]:
        total = self.useful_recall + self.missed_recall
        return {**self.__dict__, "latency_ms": {"mean": sum(self.latency_ms) / max(1, len(self.latency_ms)), "samples": len(self.latency_ms)},
                "useful_recall_rate": self.useful_recall / max(1, total),
                "acceptance": {"zero_cross_tenant_leakage": self.cross_tenant_leakage == 0,
                               "zero_harmful_recall": self.harmful_recall == 0,
                               "deletion_correct": self.deletion_failures == 0}}


def evaluate_cases(store: Any, cases: list[dict[str, Any]]) -> dict[str, Any]:
    metrics = MemoryEvaluation()
    for case in cases:
        started = time.perf_counter()
        result = store.recall(case["query"], tenant=case["tenant"], owner=case["owner"], purpose=case.get("purpose", "conversation"), now_ms=case.get("now_ms"))
        metrics.latency_ms.append((time.perf_counter() - started) * 1000)
        identifiers = {item["id"] for item in result["memories"]}; expected = set(case.get("expected_ids", [])); forbidden = set(case.get("forbidden_ids", []))
        metrics.useful_recall += len(identifiers & expected); metrics.missed_recall += len(expected - identifiers)
        metrics.intrusive_recall += len(identifiers - expected) if case.get("strict", False) else 0
        metrics.harmful_recall += len(identifiers & forbidden)
        metrics.token_overhead += len(str(result)) // 4
        if any(item.get("tenant") != case["tenant"] for item in result["memories"]): metrics.cross_tenant_leakage += 1
    return metrics.report()
