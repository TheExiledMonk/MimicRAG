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

- [x] FastAPI ingestion, document, retrieval, and answer endpoints.
- [x] Streaming generation with disconnect cancellation.
- [x] OpenAI-compatible server surface.
- [x] Background job runner, health/readiness, and index warmup.
- [x] End-to-end trace records with model/index/version provenance.
- [x] Golden-set evaluation and answer/citation quality metrics.
- [x] Rate limits, request budgets, prompt-injection boundaries, and audit events.
- [x] Configuration documentation, CLI, deployment example, and final benchmarks.

The final deterministic end-to-end benchmark over 2,000 documents and 100 queries
measured recall@1 of 1.0, retrieval p50/p95/p99 of 32.3/66.1/70.7 ms, and complete
answer-pipeline p50/p95/p99 of 32.6/69.7/74.6 ms with a zero-latency deterministic
model. These are orchestration/reference-store measurements; native MimicDB vector
performance remains covered by `mimicdb_bench_vector` and `mimicdb_bench_vector_file`.
