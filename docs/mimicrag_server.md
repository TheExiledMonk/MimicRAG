# MimicRAG server

MimicRAG combines versioned ingestion, embeddings, filtered vector search, BM25,
hybrid fusion, context construction, citations, and model generation in one service.

## Configuration

Copy `mimicrag.example.json` to `mimicrag.json`. Chat and embedding models are
configured independently. Supported provider names include `openai`, `anthropic`,
`google`, `cohere`, `ollama`, `groq`, `mistral`, `xai`, `deepseek`, `together`,
`azure_openai`, `openai_compatible`, and `custom`. For a compatible local or hosted
service, set `base_url`, `model`, and `api_key_env`.

For Azure OpenAI, set `base_url` through the deployment name (for example,
`https://RESOURCE.openai.azure.com/openai/deployments/DEPLOYMENT`) and optionally set
`api_version`; the adapter uses Azure's `api-key` header.

Secrets should be supplied through the named environment variables. Literal `api_key`
values are accepted for controlled development, but are redacted from diagnostics.

Storage backends:

- `memory`: tests and ephemeral evaluation.
- `embedded`: the in-process MimicDB C++ engine.
- `network`: a MimicDB server using `host`, `port`, and `identity_key_path`.

## Running

```bash
PYTHONPATH=api python -m mimicrag --config mimicrag.json serve
PYTHONPATH=api python -m mimicrag --config mimicrag.json ingest handbook.txt --tenant acme
PYTHONPATH=api python -m mimicrag --config mimicrag.json query "What is the policy?" --tenant acme
```

Native endpoints are `POST /v1/documents`, `POST /v1/retrieve`, `POST /v1/answers`,
`GET /v1/jobs/{id}`, and `GET /v1/traces/{id}`. Health endpoints are `/health` and
`/ready`. `POST /v1/chat/completions` implements the usual OpenAI chat response and
SSE chunk format. Supply the configured service key as `Authorization: Bearer ...`.

Ingestion publishes a complete document generation before queuing its embedding job.
Retrieval remains available lexically while the job runs. Tenant, access-scope, and
embedding-model masks are applied before vector values are scored.

## Evaluation

Golden sets are JSON arrays:

```json
[
  {
    "query": "What is the retention period?",
    "relevant_source_uris": ["file:///policy.md"],
    "required_answer_terms": ["30 days"],
    "tenant_id": "acme"
  }
]
```

Run `python -m mimicrag --config mimicrag.json evaluate golden.json`. Add `--generate`
to measure required answer terms and generated citation markers as well as retrieval
recall and reciprocal rank.

## Operational boundaries

The service enforces API authentication, per-identity sliding-window rate limits,
document/query/token budgets, tenant and access-scope isolation, and constant-time key
comparison. Retrieved evidence is serialized as untrusted JSON data beneath a fixed
system policy. Injection heuristics are recorded in append-only traces; they are signals,
not a substitute for permissions. Trace JSONL files can contain user queries and chunk
identifiers and must be protected accordingly.

HTTP disconnects close the generation iterator. OpenAI-compatible, Anthropic, and Ollama
adapters consume their native streaming formats; other adapters use a single-chunk
fallback. Provider cancellation beyond closing the local stream depends on the upstream
HTTP implementation.
