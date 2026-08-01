from __future__ import annotations

from dataclasses import dataclass
import re
import time

from .runtime import RagRuntime


@dataclass(frozen=True)
class EvaluationCase:
    query: str
    relevant_source_uris: tuple[str, ...]
    required_answer_terms: tuple[str, ...] = ()
    tenant_id: str = "default"
    access_scope: str = "public"


@dataclass(frozen=True)
class EvaluationResult:
    cases: int
    recall_at_k: float
    reciprocal_rank: float
    answer_term_accuracy: float
    citation_rate: float
    latency_ms_p50: float


def evaluate(runtime: RagRuntime, cases: list[EvaluationCase], top_k: int = 10, generate: bool = False) -> EvaluationResult:
    reciprocal, recalled, terms, cited, latencies = 0.0, 0, 0, 0, []
    for case in cases:
        started = time.perf_counter()
        hits, _ = runtime.retrieve(case.query, case.tenant_id, case.access_scope, top_k)
        sources = [runtime.store.document(hit.chunk.document_id).source_uri for hit in hits]
        ranks = [index + 1 for index, source in enumerate(sources) if source in case.relevant_source_uris]
        recalled += bool(ranks)
        reciprocal += 1.0 / min(ranks) if ranks else 0.0
        if generate:
            answer = runtime.answer(case.query, case.tenant_id, case.access_scope, top_k)
            terms += all(term.casefold() in answer.answer.casefold() for term in case.required_answer_terms)
            cited += bool(re.search(r"\[\d+\]", answer.answer))
        else:
            terms += 1
            cited += bool(hits)
        latencies.append((time.perf_counter() - started) * 1000)
    count = max(1, len(cases))
    ordered = sorted(latencies) or [0.0]
    return EvaluationResult(len(cases), recalled / count, reciprocal / count, terms / count, cited / count, ordered[len(ordered) // 2])
