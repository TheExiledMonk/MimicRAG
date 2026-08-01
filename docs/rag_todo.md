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

- [ ] Batched embedding jobs with retry, provenance, and idempotence.
- [ ] MimicDB vector dataset and linkage to published chunks.
- [ ] Native lexical/BM25 index.
- [ ] ACL and tenant predicates before vector scoring.
- [ ] Exact/IVF planner with workload-aware defaults.
- [ ] Hybrid vector + lexical retrieval and reciprocal-rank fusion.
- [ ] Optional provider/local reranking.
- [ ] Context packing, deduplication, token budget, and source citations.
- [ ] Retrieval benchmarks and recall evaluation fixtures.

## Run 3 — complete RAG server

- [ ] FastAPI ingestion, document, retrieval, and answer endpoints.
- [ ] Streaming generation with disconnect cancellation.
- [ ] OpenAI-compatible server surface where practical.
- [ ] Background job runner, health/readiness, and index warmup.
- [ ] End-to-end trace records with model/index/version provenance.
- [ ] Golden-set evaluation and answer/citation quality metrics.
- [ ] Rate limits, request budgets, prompt-injection boundaries, and audit events.
- [ ] Configuration documentation, CLI, deployment example, and final benchmarks.
