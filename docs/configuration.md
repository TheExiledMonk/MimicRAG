# MimicRAG configuration reference

The native server reads one JSON file. `chat` is required when RAG is enabled, while `embedding` is
required when either RAG or Memory is enabled; each configured model object must contain a non-empty
`model`. A Memory-only service therefore needs no chat-provider configuration, and an operational
shell with both components disabled needs neither model. Unknown top-level keys are ignored, but relying on that behavior is discouraged.
Start from [`mimicrag.example.json`](../mimicrag.example.json).

## Model objects

Both `chat` and `embedding` accept:

| Field | Meaning |
|---|---|
| `provider` | Provider adapter name; defaults to `openai_compatible` |
| `model` | Required provider model identifier |
| `base_url` | Optional override; known providers receive a default URL |
| `api_key_env` | Recommended environment variable containing the credential |
| `api_key` | Inline credential; avoid in committed or broadly readable files |
| `api_version` | Provider-specific API version, notably Azure OpenAI |
| `timeout_seconds` | Request timeout, default 60 |
| `max_retries` | Provider retry count, default 2 |
| `headers` | Additional string-valued HTTP headers |

Known URL defaults include OpenAI, Anthropic, Google, Cohere, Ollama, Groq, Mistral, xAI,
DeepSeek, Together, and MiniMax. `provider: "minimax"` uses MiniMax's recommended
Anthropic-compatible endpoint at `https://api.minimax.io/anthropic/v1`. Use
`provider: "openai_compatible"` with `base_url: "https://api.minimax.io/v1"` when OpenAI format is
specifically required. Azure and arbitrary gateways should
specify `base_url` explicitly. Anthropic-compatible gateways use `provider: "anthropic"` with a
custom `base_url` and optional headers. Anthropic and MiniMax chat models require local or another
provider's embeddings.

## `server`

Important fields are:

- Components: `rag_enabled` and `memory_enabled` independently expose the MimicRAG and
  MimicMemory HTTP surfaces. Both default to `true`. MimicDB remains independently usable through
  its native server regardless of these settings. Disabling RAG does not disable the internal
  indexing primitives Memory uses for recall.
- Network and storage: `host`, `port`, `data_path`, `max_body_bytes`, `max_query_chars`,
  `context_chars`, `top_k`, `worker_threads`, and `job_workers`.
- Limits: `requests_per_minute`, `answer_max_tokens`, `retention_days`, and capacity/memory/index
  warning byte thresholds.
- Tracing/audit: `trace_memory`, `trace_path`, `trace_max_bytes`, `audit_log_path`, and
  `audit_log_max_bytes`.
- Authentication: legacy `api_key`/`api_key_env`, or preferred named `keys` identities.

Each item in `server.keys` requires `id` plus `key` or `key_env`. It may define `permissions`
(`read`, `write`, `admin`), allowed `tenants`, allowed `scopes`, query/ingestion/provider requests
per minute, and `storage_bytes`. An empty tenant or scope list means unrestricted for that
dimension; populate the lists to enforce an allowlist.

For a Memory-only endpoint set `rag_enabled` to `false`; for RAG without agent memory set
`memory_enabled` to `false`. Disabled component routes return HTTP 503, `/health` reports the
active component set, and `/openapi.json` omits disabled routes. `/v1/retrieve/combined` is exposed
only when both components are enabled.

## `local_embedding`

Fields are `enabled`, `eager_dual_index`, `model_path`, `gpu_layers`, `threads`, `context_size`,
`document_prefix`, and `query_prefix`. `model_path` is mandatory when enabled. A thread count of
zero lets the runtime select a value; GPU layers `-1` requests maximum offload with CPU fallback.
Query and document prefixes are part of embedding identity and changing them triggers safe
reindexing.

## `ingestion`

Chunk budgets are controlled by `default_mode`, `target_chars`, `minimum_chars`, `maximum_chars`,
`overlap_chars`, and `maximum_chunks`. Structured/semantic resource limits include
`maximum_analysis_calls`, `maximum_analysis_input_chars`, `maximum_generated_metadata_bytes`,
`maximum_graph_edges`, and `maximum_analysis_seconds`.

Set `analysis_enabled=true` and `analysis_use_chat_provider=true` to allow bounded semantic analysis.
`prompt_version` participates in ingestion identity; change it intentionally when prompt behavior
changes.

## `retrieval`

Feature toggles are `classification_enabled`, `rewriting_enabled`, and `reranking_enabled`.
Resource/quality controls are `shortlist_multiplier`, `maximum_rewrites`, `maximum_rewrite_chars`,
`rerank_weight`, `recency_weight`, `authority_weight`, `source_quality_weight`, `feedback_weight`,
`minimum_confidence`, and `near_duplicate_threshold`.

## `graph`

Fields are `enabled`, `max_seeds`, `max_neighbors`, `max_section_children`, and `min_seed_score`.
These are translated into the native server graph limits.

## Companion configuration

V1.4–V1.6 packages do not read additional sections from this native JSON file:

- `mimicrag_ingestion` receives server URL/key, source credentials, routes, and state paths through
  CLI arguments or Python constructors.
- `mimicrag_dev` reads `MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY` for client/MCP operations.
- `mimicrag_memory` receives its SQLite path, `MemoryPolicy`, memory model, and optional RAG client
  through Python or CLI arguments.

Keep credentials in environment variables or the owning platform's secret store. Configuration
changes affecting parser, chunker, prompt, prefixes, or embedding model identity can cause a safe
reindex; rehearse them against a copied data directory before production rollout.
