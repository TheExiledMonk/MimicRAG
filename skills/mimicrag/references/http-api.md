# MimicRAG HTTP API reference

All routes accept JSON over HTTP. When configured, send
`Authorization: Bearer $MIMICRAG_API_KEY`. Keep the server behind TLS or on a trusted private
network; its native listener does not terminate TLS.

## Health

`GET /health` and `GET /ready` return runtime, storage, and index state.

## Retrieve

`POST /v1/retrieve`

```json
{
  "query": "What is the retention policy?",
  "tenant_id": "acme",
  "access_scope": "employees",
  "top_k": 5,
  "graph_enabled": true
}
```

The response contains ranked `hits`. Preserve each hit's `node_id`, content, score, source URI,
document identity, and metadata. The server applies tenant and exact access-scope filtering before
scoring candidates.

## Generate an answer

`POST /v1/answers` accepts the retrieval fields plus optional `conversation`, `options`, and
`stream`. A non-streaming response includes `answer`, `citations`, `trace_id`, and the embedding
backend. With `stream: true`, parse server-sent events until `data: [DONE]`.

## OpenAI-compatible chat

`POST /v1/chat/completions`

```json
{
  "model": "configured-model",
  "messages": [{"role": "user", "content": "What is the retention policy?"}],
  "tenant_id": "acme",
  "access_scope": "employees",
  "top_k": 5,
  "stream": false
}
```

Standard chat fields `max_tokens`, `temperature`, `top_p`, and `stop` are forwarded. The
non-streaming response adds a `mimicrag` object containing trace, citation, and embedding metadata.

## Expand related nodes

`POST /v1/graph/expand`

```json
{
  "node_id": "node-id-from-a-hit",
  "tenant_id": "acme",
  "access_scope": "employees",
  "max_neighbors": 16
}
```

Use only node IDs returned under the same tenant and access scope.

## Ingest a document

`POST /v1/documents`

```json
{
  "text": "Document body",
  "source_uri": "docs://policy/retention",
  "document_id": "retention-policy",
  "title": "Retention policy",
  "tenant_id": "acme",
  "access_scope": "employees",
  "metadata": {"department": "legal"},
  "background": false
}
```

Omit `document_id` only when server-derived stable identity is acceptable. With
`background: true`, poll `GET /v1/jobs/{id}` using the returned job ID.

## Evaluation and traces

- `POST /v1/evaluations`: run a golden set; items can specify `query`, relevant source URIs, and
  required answer terms.
- `GET /v1/traces?limit=N`: list recent traces, capped by the server.
- `GET /v1/traces/{id}`: inspect one trace.

## Error behavior

Expect `400` for invalid input, `401` for authentication failure, `404` for unknown resources,
`429` for rate limiting, and `503` when the bounded work queue is full.
