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

Set `embedding.provider` to `local` when the local model is primary. This bypasses
remote probing entirely; other provider values retain automatic remote-to-local
failover.

## Failover safety

Remote and local embeddings live in independent native vector spaces. With
`eager_dual_index`, every published chunk is embedded into both spaces. Queries use
remote embeddings while healthy, switch to the matching pre-populated local space on
failure, and use BM25 if no compatible vector space is ready. Vectors from different
models or prefix configurations are never mixed.

The append-only `catalog.mrg` stores Zstd-compressed document metadata/text followed
by contiguous float32 vector blocks. Every versioned record carries dimensions,
lengths, and a checksum; replay truncates a partially written final record to the last
verified boundary. If only the former `catalog.jsonl` exists, startup converts it to a
temporary binary catalog and atomically publishes `catalog.mrg` after every legacy
record has replayed successfully. The legacy file is left untouched for explicit
operator removal after verification.

Catalog replay restores both configured spaces. Persisted IVF vector blocks and the
compact BM25 dictionary/postings are memory-mapped when their catalog generation and
checksums match; stale or missing derived indexes are rebuilt. Sealed duplicate vector
columns are released after IVF publication, article text remains disk-backed, and
document metadata is interned once per version. Tenant and access-scope predicates are
applied by the MimicDB engine before vectors are loaded and scored; superseded document
versions are removed before fusion. Newly appended BM25 rows use a small heap delta
until the next persisted-index rebuild.

Endpoints are `/health`, `/ready`, `/v1/documents`, `/v1/retrieve`, `/v1/answers`, and
OpenAI-compatible `/v1/chat/completions`. The HTTP server uses a bounded native worker
pool, request-size limits, bearer authentication, constant-time key comparison, and
rate limiting.

Native parity endpoints also include `GET /v1/jobs/{id}`, `GET /v1/traces/{id}`,
`GET /v1/traces?limit=N`, and `POST /v1/evaluations`. Answer and OpenAI-compatible
streams forward provider tokens immediately over SSE. Prompt-injection assessments,
retrieval details, provider/model identity, latency, and citations are recorded in the
bounded trace store and JSONL trace file.

Lifecycle operations include tenant-bound `DELETE /v1/documents/{id}` (with a JSON
`tenant_id`) and `GET /v1/storage`. Native `inspect` and `compact` commands validate the
versioned catalog and atomically retain only current document generations. Compaction is an
offline maintenance command; it removes derived indexes and rebuilds them on the next start.

Retrieved chunks include a stable `node_id`. `POST /v1/graph/expand` accepts that ID,
the tenant/access scope, and a neighbor limit, returning typed document, section, and
chunk nodes without another embedding or global search. Every returned node has its own
stable ID, allowing an agent to move upward, downward, or sideways through bounded,
iterative deep dives. The initial search
uses at most five seeds and 32 graph candidates by default; graph timings and retained
hits are included in retrieval responses and traces. Set `graph_enabled:false` per
request or `graph.enabled:false` globally to bypass the stage.

The executable replaces the Python CLI as well:

```bash
mimicrag_server ingest document.txt --config mimicrag.json --tenant acme
mimicrag_server query "What does the document say?" --config mimicrag.json --tenant acme
mimicrag_server evaluate golden-set.json --config mimicrag.json
mimicrag_server --config mimicrag.json serve
```

## Wikipedia ingestion

The native importer streams either plain XML or Wikimedia's concatenated multistream
BZip2 archive without materializing the decompressed dump. It keeps main-namespace
articles, skips redirects, preserves headings for structural graph construction, and
stores stable `enwiki:<page-id>` document IDs plus canonical source URLs and licence
metadata.

Start with a parser-only validation, then a bounded embedded pilot:

```bash
mimicrag_server wiki-ingest /data/enwiki-latest-pages-articles-multistream.xml.bz2 \
  --config mimicrag.json --limit 10000 --dry-run --no-resume

mimicrag_server wiki-ingest /data/enwiki-latest-pages-articles-multistream.xml.bz2 \
  --config mimicrag.json --limit 10000 --tenant wikipedia \
  --checkpoint /data/mimicrag/enwiki.checkpoint.json
```

`--progress N` controls progress reports, `--skip N` supports bounded sampling, and
`--no-resume` ignores an existing checkpoint. Checkpoints are atomically replaced
after every progress interval and on clean completion. Restarting with the same
checkpoint scans forward to the last committed page and relies on stable page IDs and
idempotent document versions to avoid duplicate publication.
