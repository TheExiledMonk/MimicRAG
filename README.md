# MimicDB / MimicRAG

MimicRAG is a self-contained native C++ RAG runtime built on the MimicDB columnar and
vector engine. One executable handles document ingestion, chunking, local or remote
embeddings, hybrid BM25/vector retrieval, metadata filtering, document-graph
navigation, cited answer generation, tracing, and evaluation.

It does not require a Python runtime, separate vector database, search engine, graph
database, or embedding server. Remote model APIs remain optional: embeddings can run
in-process through the pinned `llama.cpp` submodule with Vulkan, CUDA, Metal, or CPU.

MimicDB, the underlying engine, is also available as a standalone columnar database
with a native server, C++ API, compatibility layers, and management UI.

## Why MimicRAG

- One C++ executable and one data directory
- Dense vector search plus exact BM25 with reciprocal-rank fusion
- Custom IVF routing with adaptive shortlist and exact noise fallback
- Predicate filtering before vector values are loaded and scored
- Local GGUF embeddings on GPU with CPU fallback
- OpenAI, Anthropic, Google, Cohere, Ollama, Azure OpenAI, and OpenAI-compatible chat
- Custom provider URLs, model names, headers, and environment-based API keys
- Tenant and access-scope filtering
- Document, section, and chunk graph traversal for iterative research
- OpenAI-compatible chat-completions endpoint and SSE streaming
- Citations, bounded traces, background ingestion jobs, and golden-set evaluation
- Disk-backed content and memory-mapped vector and lexical indexes
- Append-only, checksummed binary catalog with partial-tail recovery

## Architecture

```text
documents / Wikipedia dump
          |
          v
  native chunker + graph builder
          |
          +---- local llama.cpp embeddings ----+
          |                                     |
          +---- remote embeddings (optional)    |
                                                v
                     +--------------------------+------------------+
                     | MimicDB vector engine + mapped custom IVF   |
                     | mapped BM25 + tenant/access predicates       |
                     | reciprocal-rank fusion + bounded graph walk  |
                     +--------------------------+------------------+
                                                |
                         disk-backed winning passages only
                                                |
                                                v
                           cited answer / OpenAI-compatible stream
```

Search-critical structures are compact or memory-mapped. Article text is addressed by
offset and read only for selected results. Document metadata is interned once per
document version, and newly ingested lexical terms use a small heap overlay until the
next persisted-index rebuild.

## Current benchmark

Measured on an AMD Ryzen 9 7950X3D and Radeon RX 7900 XTX using Vulkan, local Nomic
Embed Text v1.5 embeddings, 10,000 real English Wikipedia articles, 148,671 chunks,
768-dimensional vectors, and 930,440 graph edges:

| Measurement | Result |
|---|---:|
| Warm startup | 4.31 s |
| Ready-state RSS | 1.25 GiB |
| Private/anonymous memory | 570 MiB |
| Sequential end-to-end retrieval | 44.46 QPS |
| Sequential average / p95 | 18.63 / 20.39 ms |
| Eight-client retrieval | 249–256 QPS |
| Eight-client p95 | 31–36 ms |

These are end-to-end loopback HTTP results including local query embedding, BM25,
vector search, filtering, fusion, graph expansion, passage loading, and JSON encoding.
They are not directly comparable to ANN-only benchmarks that receive a precomputed
query vector. Full methodology is in
[`benchmarks/results/mimicrag_wikipedia_10000_2026-08-02.md`](benchmarks/results/mimicrag_wikipedia_10000_2026-08-02.md).

## Status

MimicRAG is suitable for evaluation and, with a commercial license, controlled single-node
commercial deployments.
It is not yet a distributed or high-availability database. Before using it for a
mission-critical public service, put TLS in front of it and establish backups,
monitoring, capacity limits, relevance tests, and recovery drills.

## Prerequisites

- Linux, macOS, or another C++20 platform
- CMake 3.20 or newer
- A C++20 compiler
- Git and `pkg-config`
- libcurl, BZip2, Zstandard, and nlohmann-json development packages
- Optional Vulkan/CUDA/Metal development stack for GPU embeddings

Example Ubuntu/Debian CPU build dependencies:

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config curl \
  libcurl4-openssl-dev libbz2-dev libzstd-dev nlohmann-json3-dev
```

For Vulkan builds, also install your vendor driver and distribution Vulkan development
packages (commonly `libvulkan-dev`, `glslc`, and `libshaderc-dev`).

## Clone and build

Clone with the pinned `llama.cpp` submodule:

```bash
git clone --recurse-submodules https://github.com/TheExiledMonk/MimicRAG.git
cd MimicRAG
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

Build an optimized native server with automatic GPU backend detection:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMIMICRAG_ENABLE_LLAMA=ON \
  -DMIMICRAG_LLAMA_GPU=auto \
  -DMIMICDB_NATIVE_ARCH=ON
cmake --build build-release -j --target mimicrag_server
```

The executable is `build-release/rag_cpp/mimicrag_server`.

GPU selection can be forced with `-DMIMICRAG_LLAMA_GPU=vulkan`, `cuda`, `metal`, or
`cpu`. To build without in-process llama.cpp support, set
`-DMIMICRAG_ENABLE_LLAMA=OFF`; remote embeddings or BM25-only fallback remain
available.

## Download a local embedding model

The downloader selects a larger Qwen embedding model on GPUs with at least 5 GiB VRAM
and a smaller Nomic model on CPU or lower-memory hosts:

```bash
scripts/download_embedding_model.sh
```

Override detection when needed:

```bash
scripts/download_embedding_model.sh --gpu
scripts/download_embedding_model.sh --cpu
scripts/download_embedding_model.sh --output-dir /var/lib/mimicrag/models
```

Downloads are resumable and validated for minimum size and GGUF magic. The script also
writes `mimicrag.local_embedding.json`; copy its `local_embedding` object into your
runtime configuration.

## Configure

```bash
cp mimicrag.example.json mimicrag.json
```

Edit `mimicrag.json` so `local_embedding.model_path` matches the downloaded model.
Keep secrets in environment variables, not JSON or Git:

```bash
export MIMICRAG_API_KEY='replace-with-a-long-random-server-key'
export OPENAI_API_KEY='provider-key-if-used'
```

Set `embedding.provider` to `local` to make the embedded GGUF model primary. With a
remote embedding provider and `local_embedding.eager_dual_index=true`, MimicRAG keeps
separate compatible vector spaces and automatically falls back to the local model.
Anthropic does not provide embeddings, so use the local fallback or another embedding
provider with Anthropic chat.

Recognized provider defaults include `openai`, `anthropic`, `google`, `cohere`,
`ollama`, `groq`, `mistral`, `xai`, `deepseek`, and `together`. Use
`openai_compatible` plus `base_url` for vLLM, llama.cpp server, MiniMax-compatible
gateways, or other compatible services. Azure OpenAI uses `azure_openai` and
`api_version`.

## Run

```bash
./build-release/rag_cpp/mimicrag_server serve --config mimicrag.json
```

Equivalent positional configuration syntax is supported:

```bash
./build-release/rag_cpp/mimicrag_server mimicrag.json
```

Check readiness:

```bash
curl -fsS http://127.0.0.1:8080/health \
  -H "Authorization: Bearer $MIMICRAG_API_KEY"
```

## Ingest and retrieve

Ingest a local document:

```bash
./build-release/rag_cpp/mimicrag_server ingest handbook.md \
  --config mimicrag.json \
  --tenant acme \
  --source-uri file:///knowledge/handbook.md \
  --title 'Company handbook'
```

Or use HTTP:

```bash
curl -fsS http://127.0.0.1:8080/v1/documents \
  -H "Authorization: Bearer $MIMICRAG_API_KEY" \
  -H 'Content-Type: application/json' \
  --data '{
    "text":"MimicRAG combines vector, lexical, and graph retrieval.",
    "source_uri":"docs://overview",
    "title":"Overview",
    "tenant_id":"acme",
    "access_scope":"engineering"
  }'
```

Retrieve evidence without generating an answer:

```bash
curl -fsS http://127.0.0.1:8080/v1/retrieve \
  -H "Authorization: Bearer $MIMICRAG_API_KEY" \
  -H 'Content-Type: application/json' \
  --data '{
    "query":"How does retrieval work?",
    "tenant_id":"acme",
    "access_scope":"engineering",
    "top_k":5
  }'
```

Generate a cited answer:

```bash
curl -fsS http://127.0.0.1:8080/v1/answers \
  -H "Authorization: Bearer $MIMICRAG_API_KEY" \
  -H 'Content-Type: application/json' \
  --data '{
    "query":"How does retrieval work?",
    "tenant_id":"acme",
    "access_scope":"engineering",
    "top_k":5
  }'
```

The `/v1/chat/completions` route accepts OpenAI-style messages and supports
`"stream":true` over server-sent events. Its response includes MimicRAG trace,
citation, and embedding-backend metadata.

## Graph deep dives

Every retrieval hit contains a stable `node_id`. Explore its document structure
without another global vector search:

```bash
curl -fsS http://127.0.0.1:8080/v1/graph/expand \
  -H "Authorization: Bearer $MIMICRAG_API_KEY" \
  -H 'Content-Type: application/json' \
  --data '{
    "node_id":"NODE_ID_FROM_RETRIEVAL",
    "tenant_id":"acme",
    "access_scope":"engineering",
    "max_neighbors":16
  }'
```

Returned document, section, and chunk nodes are individually expandable, enabling an
agent to move upward, downward, and sideways through related evidence.

## Wikipedia ingestion

The native importer streams plain XML or Wikimedia multistream BZip2 dumps without
materializing the decompressed file. Validate parsing first:

```bash
./build-release/rag_cpp/mimicrag_server wiki-ingest /data/enwiki.xml.bz2 \
  --config mimicrag.json --limit 10000 --dry-run --no-resume
```

Then ingest with a resumable checkpoint:

```bash
./build-release/rag_cpp/mimicrag_server wiki-ingest /data/enwiki.xml.bz2 \
  --config mimicrag.json \
  --tenant wikipedia \
  --limit 10000 \
  --checkpoint /var/lib/mimicrag/enwiki.checkpoint.json
```

The importer retains main-namespace non-redirect articles and preserves headings for
graph construction.

## API summary

| Method | Route | Purpose |
|---|---|---|
| `GET` | `/health`, `/ready` | Readiness and index/storage state |
| `POST` | `/v1/documents` | Versioned document ingestion |
| `DELETE` | `/v1/documents/{id}` | Tenant-bound document deletion |
| `GET` | `/v1/storage` | Live and reclaimable storage statistics |
| `POST` | `/v1/retrieve` | Hybrid evidence retrieval |
| `POST` | `/v1/answers` | Cited RAG answer, optionally SSE |
| `POST` | `/v1/chat/completions` | OpenAI-compatible chat endpoint |
| `POST` | `/v1/graph/expand` | Bounded structural deep dive |
| `POST` | `/v1/evaluations` | Golden-set evaluation |
| `GET` | `/v1/jobs/{id}` | Background-ingestion job state |
| `GET` | `/v1/traces`, `/v1/traces/{id}` | Retrieval and answer traces |
| `DELETE` | `/v1/tenants/{id}` | Verified tenant erasure |
| `POST` | `/v1/maintenance/retention` | Apply retention policy |
| `POST` | `/v1/maintenance/compact` | Online atomic compaction and rebuild |
| `GET` | `/metrics` | Prometheus operational metrics |

## Persistence

The configured `server.data_path` contains all runtime state:

- `catalog.mrg`: append-only, checksummed catalog with compressed metadata and vectors
- `content.dat`: disk-backed chunk text addressed by byte offsets
- `content.manifest`: content/catalog generation match
- `local.ivf` / `remote.ivf`: persisted, memory-mapped vector indexes
- `lexical.idx`: compact memory-mapped BM25 dictionary, lengths, and postings
- trace JSONL and optional Wikipedia checkpoint

Back up the whole directory as one consistency unit while ingestion is stopped. The
catalog can recover a truncated final append and stale derived indexes are rebuilt, but
this is not a substitute for tested backups.

## Production deployment

A sample unit is provided at [`deploy/mimicrag_cpp.service`](deploy/mimicrag_cpp.service),
with a complete installation, TLS, backup, restore, upgrade, and monitoring guide in
[`docs/deployment.md`](docs/deployment.md).
Adjust its paths, copy configuration to `/etc/mimicdb/mimicrag.json`, put secrets in
`/etc/mimicdb/mimicrag.env`, and make `server.data_path` writable by the service user.

The native HTTP listener is IPv4 HTTP, not TLS. Bind to loopback or a private network
and place a TLS reverse proxy or load balancer in front of it. Set `server.api_key_env`,
restrict filesystem permissions, and never commit provider keys.

Recommended production checks:

- Backup and restore rehearsal
- Golden-query relevance regression suite
- Sustained ingestion/query soak test
- Memory, disk, latency, error-rate, and provider monitoring
- Per-tenant rate and storage policy
- Reverse-proxy request limits and TLS
- API-key rotation and incident procedure

## Tests

```bash
ctest --test-dir build-release --output-on-failure
```

Focused native RAG tests include catalog recovery, Wikipedia parsing, HTTP retrieval,
graph expansion, provider streaming, evaluation, and vector-index persistence. Some
legacy CLI/API tests require the optional Python dependencies in `requirements.txt`.

## Repository layout

- `rag_cpp/`: native MimicRAG executable and HTTP API
- `engine/`: MimicDB columnar, SIMD, vector, IVF, and Vulkan engine
- `server/`: MimicDB binary-protocol database server
- `api_cpp/`: C++ API and compatibility helpers
- `client/`, `api/`: legacy/reference Python clients and adapters
- `management_ui/`: optional management service and desktop/web UI
- `benchmarks/`: engine and RAG benchmarks with recorded results
- `docs/`: design, security, API, and operational documentation
- `scripts/`: model download and smoke-test helpers
- `deploy/`: deployment templates

## MimicDB engine

The underlying MimicDB engine remains independently usable for append-oriented
columnar workloads. It includes mask-based predicates, deterministic aggregates,
segment persistence/compression, SIMD execution, a custom binary server protocol,
vector search, adaptive multicore execution, and optional Vulkan residency. See
[`docs/design.md`](docs/design.md), [`docs/mimicapi.md`](docs/mimicapi.md), and
[`docs/security_v1.md`](docs/security_v1.md).

## Limitations

- Single-node runtime; no replication or automatic failover
- Append/version model rather than general transactions and arbitrary updates
- Native HTTP API supports bearer authentication but does not terminate TLS
- Large-corpus recall and performance still require workload-specific validation
- Index formats are versioned but rolling-upgrade tooling is not yet complete
- Not a general-purpose graph-query engine

## License

MimicDB and MimicRAG are source-available under the
[PolyForm Noncommercial License 1.0.0](LICENSE). Non-commercial use, modification, and
distribution are permitted under its terms. **Commercial use requires a separate license.**
See [COMMERCIAL_LICENSE.md](COMMERCIAL_LICENSE.md) for commercial licensing inquiries.

This is a source-available license, not an OSI-approved open-source license.
Third-party dependencies and the `llama.cpp` submodule remain under their respective licenses.

## Further documentation

- [`roadmap.md`](roadmap.md): prioritized post-V1 development roadmap
- [`docs/mimicrag_cpp.md`](docs/mimicrag_cpp.md): native runtime details
- [`docs/agent-integration.md`](docs/agent-integration.md): OpenClaw, Hermes, and generic agent integration
- [`docs/design.md`](docs/design.md): MimicDB architecture and durability
- [`docs/security_v1.md`](docs/security_v1.md): database protocol security
- [`docs/security_setup_howto.md`](docs/security_setup_howto.md): security setup
- [`benchmarks/results/`](benchmarks/results/): reproducible benchmark records

## Agent skill

The portable [`skills/mimicrag/SKILL.md`](skills/mimicrag/SKILL.md) teaches compatible agents how
to retrieve evidence, generate cited answers, ingest approved documents, and perform bounded graph
deep dives. Install the complete `skills/mimicrag` directory so its HTTP reference is included.

For Hermes Agent after this repository is public:

```bash
hermes skills install TheExiledMonk/MimicRAG/skills/mimicrag
```

For OpenClaw, copy `skills/mimicrag` beneath a configured workspace or global skills root. See the
[agent integration guide](docs/agent-integration.md) for credentials, tool boundaries, and testing.
