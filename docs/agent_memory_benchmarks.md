# Agent memory benchmark suite

`benchmarks/bench_agent_memory.py` is the release benchmark for the V1.8 memory system. Its
default mode is deterministic, offline, and requires no LLM.

```bash
PYTHONPATH=api python benchmarks/bench_agent_memory.py \
  --memories 500 --queries 100 \
  --output benchmarks/results/agent_memory.json
```

It measures ingestion throughput, recall p50/p95/p99 and throughput, retrieval quality, database
size, bytes per memory, and peak resident memory. It asserts evidence binding; tenant/owner
isolation; sensitivity, confirmation, quarantine, and rejection boundaries; correction precedence;
stale suppression; reinforcement and relations; reminders; working-memory promotion; document
authority; export/audit/deletion; local extraction/cache; and durable jobs/retries/dead letters.

## Native HTTP

Run against a disposable authenticated native server with:

```bash
--native-url http://127.0.0.1:8080 --api-key "$MIMICRAG_API_KEY"
```

This adds native evidence, recall latency, export, and deletion checks under tenant
`benchmark-native`.

## Live LLMs

Live extraction contract and latency measurements are opt-in and incur provider charges:

```bash
ANTHROPIC_API_KEY=... ANTHROPIC_MODEL=claude-haiku-4-5 \
PYTHONPATH=api python benchmarks/bench_agent_memory.py --live-llm

MINIMAX_API_KEY=... MINIMAX_MODEL=MiniMax-M2.7 \
PYTHONPATH=api python benchmarks/bench_agent_memory.py --live-llm
```

MiniMax uses its preferred Anthropic-compatible API. Add `--minimax-openai` to test the optional
OpenAI-compatible endpoint. The default is three calls per configured adapter; use `--llm-runs`
to change it.
