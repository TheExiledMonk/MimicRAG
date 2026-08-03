# Semantic ingestion V1.2

MimicRAG accepts plain text, Markdown, and HTML and normalizes them into source-addressed blocks.
The normalized model retains headings and section hierarchy, paragraphs, lists, tables, code,
captions, footnotes, citations, pages, and exact character spans. Generated context is advisory;
stored source text remains the citation authority.

Set `format` to `text`, `markdown`, or `html` on `POST /v1/documents`. Select one of three modes:

- `fast` preserves the deterministic fixed-window V1.1 path and is the default.
- `structured` groups blocks on document boundaries, attaches small fragments, and only overlaps
  dense subdivisions.
- `semantic` starts with structured chunks and selectively asks the configured chat provider about
  dense prose, tables, or mixed-topic candidates. Invalid, failed, over-budget, or timed-out analysis
  falls back deterministically.

```json
{
  "tenant_id": "docs",
  "source_uri": "file:///manual.md",
  "format": "markdown",
  "mode": "semantic",
  "background": true,
  "text": "# Manual\n\nSource text..."
}
```

Semantic analysis is opt-in with `ingestion.analysis_enabled`. It uses the existing `chat` provider,
including OpenAI-compatible custom URLs and Ollama. Local runtimes inherit their configured GPU
acceleration and CPU fallback. API keys remain environment-resolved and are never included in model
messages. The fixed system policy treats document content as untrusted data and validates the JSON
response before accepting split proposals or contextual headers.

Budgets bound chunk count, analysis calls/input/time, generated metadata, and graph edges. Provider
timeouts and `max_retries` are honored. Analysis decisions record cache status, model and prompt
identity, latency, and estimated tokens. Cache keys include content and configuration. Parser,
chunker, prompt, analysis-model, and embedding identities participate in version identity so a
configuration change safely creates a new indexed version.

Background requests return a job ID. `GET /v1/jobs/{id}` reports `status`, `stage`, and `progress`;
`DELETE /v1/jobs/{id}` requests cooperative cancellation. Queued requests are checkpointed beneath
the data directory and automatically resubmitted after restart. Cancellation is checked between
parse, analysis, embedding, and publish stages; an in-flight provider call completes or times out
before cancellation is observed.
