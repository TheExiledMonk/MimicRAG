# MimicRAG implementation plan

MimicRAG is delivered in three implementation runs. The database owns document truth,
version visibility, access metadata, retrieval, and evidence. Model providers remain
replaceable execution dependencies.

## Run 1 — foundation and ingestion

- [x] Provider-neutral chat and embedding configuration.
- [x] OpenAI, Anthropic, Google Gemini, Cohere, Ollama, Groq, Mistral, xAI,
  DeepSeek, Together, Azure/OpenAI-compatible, and arbitrary custom endpoint
  adapters.
- [x] Configurable URL, model, headers, timeout, retries, and API key/environment secret.
- [x] Redacted configuration diagnostics.
- [x] Document, version, chunk, and publication records.
- [x] Append-only atomic generation publication.
- [x] Deterministic document identity and content-hash idempotence.
- [x] Boundary-aware overlapping text chunker.
- [x] Embedded/in-memory and MimicDB-backed stores.
- [x] Failure and visibility tests.

## Run 2 — retrieval pipeline

- [x] Batched embedding jobs with retry, provenance, and idempotence.
- [x] MimicDB vector dataset and linkage to published chunks.
- [x] Built-in lexical/BM25 index.
- [x] ACL, embedding-model, and tenant predicate masks before vector scoring.
- [x] Exact/IVF planner with workload-aware defaults.
- [x] Hybrid vector + lexical retrieval and reciprocal-rank fusion.
- [x] Optional provider/local reranking.
- [x] Context packing, deduplication, token budget, and source citations.
- [x] Retrieval benchmarks and recall evaluation fixtures.

The deterministic reference benchmark (`benchmarks/bench_rag.py`) reached recall@1
of 1.0 for 100 queries over 2,000 documents. Its in-memory brute-force reference
backend measured 31.9 ms median, 63.4 ms p95, and 120.1 ms p99. Production vector
retrieval uses MimicDB's optimized exact/IVF path; the reference numbers isolate the
Python orchestration and correctness fixture rather than replacing the native vector
benchmark.

## Run 3 — complete RAG server

- [ ] FastAPI ingestion, document, retrieval, and answer endpoints.
- [ ] Streaming generation with disconnect cancellation.
- [ ] OpenAI-compatible server surface where practical.
- [ ] Background job runner, health/readiness, and index warmup.
- [ ] End-to-end trace records with model/index/version provenance.
- [ ] Golden-set evaluation and answer/citation quality metrics.
- [ ] Rate limits, request budgets, prompt-injection boundaries, and audit events.
- [ ] Configuration documentation, CLI, deployment example, and final benchmarks.
