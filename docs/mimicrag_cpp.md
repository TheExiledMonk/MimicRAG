# Native C++ MimicRAG

`mimicrag_server` is the Python-free RAG runtime. HTTP parsing, provider requests,
chunking, BM25, reciprocal-rank fusion, context assembly, catalog replay, access
filtering, and vector search all execute in C++. Vector scoring uses the existing
MimicDB SIMD/multicore/IVF/Vulkan engine.

## Clone and build

```bash
git clone --recurse-submodules REPOSITORY_URL
cmake -S . -B build -DMIMICRAG_ENABLE_LLAMA=ON -DMIMICRAG_LLAMA_GPU=auto
cmake --build build -j --target mimicrag_server
```

`llama.cpp` is pinned as `external/llama.cpp`. If it is absent, CMake runs
`git submodule update --init --depth 1 external/llama.cpp`. GPU backend selection
prefers CUDA, then Vulkan, then Metal, with the llama.cpp CPU backend retained for
runtime fallback. It can be forced with `MIMICRAG_LLAMA_GPU=cuda`, `vulkan`, `metal`,
or `cpu`.

## Local embedding model

```bash
scripts/download_embedding_model.sh
```

The script checks NVIDIA or Linux DRM VRAM. Hosts with at least 5 GiB receive the
official Qwen3-Embedding-4B Q4_K_M GGUF; CPU-only or smaller-memory hosts receive
Nomic Embed Text v1.5 Q4_K_M. `--gpu` and `--cpu` override detection. Downloads are
resumable and are rejected unless their size and GGUF magic are valid. The generated
JSON fragment includes the model-specific query/document prefixes.

Copy its `local_embedding` object into `mimicrag.json`, then run:

```bash
./build/rag_cpp/mimicrag_server mimicrag.json
```

## Failover safety

Remote and local embeddings live in independent native vector spaces. With
`eager_dual_index`, every published chunk is embedded into both spaces. Queries use
remote embeddings while healthy, switch to the matching pre-populated local space on
failure, and use BM25 if no compatible vector space is ready. Vectors from different
models or prefix configurations are never mixed.

The append-only `catalog.jsonl` is replayed on restart and rebuilds both configured
spaces. Tenant and access-scope predicates are applied by the MimicDB engine before
vectors are loaded and scored; superseded document versions are removed before fusion.

Endpoints are `/health`, `/ready`, `/v1/documents`, `/v1/retrieve`, `/v1/answers`, and
OpenAI-compatible `/v1/chat/completions`. The HTTP server uses a bounded native worker
pool, request-size limits, bearer authentication, constant-time key comparison, and
rate limiting.

Native parity endpoints also include `GET /v1/jobs/{id}`, `GET /v1/traces/{id}`,
`GET /v1/traces?limit=N`, and `POST /v1/evaluations`. Answer and OpenAI-compatible
streams forward provider tokens immediately over SSE. Prompt-injection assessments,
retrieval details, provider/model identity, latency, and citations are recorded in the
bounded trace store and JSONL trace file.

The executable replaces the Python CLI as well:

```bash
mimicrag_server ingest document.txt --config mimicrag.json --tenant acme
mimicrag_server query "What does the document say?" --config mimicrag.json --tenant acme
mimicrag_server evaluate golden-set.json --config mimicrag.json
mimicrag_server --config mimicrag.json serve
```
