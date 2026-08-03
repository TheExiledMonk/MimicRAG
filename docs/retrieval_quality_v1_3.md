# Retrieval quality V1.3

V1.3 adds an explainable, bounded retrieval planner. Each response includes `query_plan`,
`confidence`, and `insufficient_evidence`. The planner classifies exact/identifier searches as
lexical, conceptual questions as semantic, relationship questions as graph-oriented, and all other
queries as hybrid. It expands a small built-in terminology list and can resolve bounded follow-up
pronouns from the supplied conversation. No model call is required for planning.

Only the final shortlist is reranked. Native token coverage over titles, section paths, contextual
headers, and source text is combined with reciprocal-rank fusion. Optional `authority` and
`source_quality` metadata (0–1), ingestion recency, and accepted relevance feedback provide bounded
boosts. Graph expansion runs automatically only for graph-oriented queries unless
`graph_enabled` is explicitly supplied.

## Metadata predicates

`POST /v1/retrieve` accepts a `filter`. Leaf predicates use `field`, `op`, and `value`; nested paths
use dots. Supported operations are `eq`, `ne`, `in`, `contains`, `gt`, `gte`, `lt`, and `lte`.
Compound nodes use `and`, `or`, or `not`, with bounded depth and width.

```json
{"query":"current retention policy","tenant_id":"docs","filter":{"and":[
  {"field":"category","op":"eq","value":"policy"},
  {"field":"year","op":"gte","value":2025},
  {"field":"regions","op":"contains","value":"EU"}
]}}
```

## Feedback and verification

`POST /v1/feedback` accepts a tenant-visible `chunk_id`, `relevant`, and optional `trace_id`,
`query`, and `reason`. Events are appended to `relevance_feedback.jsonl`; their bounded net score is
reported as an offline-tuning recommendation and applied to subsequent shortlists. Generated answers
are checked sentence-by-sentence for valid citations and source-token overlap. Low-confidence or empty
retrieval returns an explicit insufficient-evidence answer without spending a chat-provider call.

## Evaluation

Evaluation now reports Recall@k, MRR, nDCG@k, answer and citation correctness,
insufficient-evidence accuracy, p50/p95 latency, throughput, index bytes, and provider calls. Pass
`compare_modes: ["fast","structured","semantic"]` to evaluate each ingestion mode using the same
cases. The representative corpus in `tests/mimicrag_v13_quality_set.json` covers prose, manuals,
policies, tables, code, cross-section questions, and insufficient evidence.
