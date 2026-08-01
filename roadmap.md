# MimicDB / MimicRAG roadmap

This roadmap begins after the V1.0 release. Priorities may change based on production evidence, but
new work should preserve the core proposition: one fast native package, predictable resource use,
hardware-aware execution, and no mandatory external retrieval services.

## Engineering principles

- Keep the single-node, single-package path first-class as distributed features are added.
- Preserve deterministic CPU fallbacks for optional GPU and model-assisted features.
- Measure end-to-end retrieval, not isolated kernels alone.
- Retain optimizations only when representative benchmarks show a repeatable improvement.
- Keep source text and provenance authoritative; generated metadata is advisory.
- Apply authorization filters before loading or scoring protected content.
- Avoid meaningful regressions in the existing fast retrieval path.

## V1.1: operational maturity

### Data lifecycle

- [ ] Add document deletion and replacement APIs with removal from vector, lexical, graph, content,
  and trace references.
- [ ] Support retention policies and verifiable tenant data erasure.
- [ ] Add online compaction for superseded versions, tombstones, content storage, and indexes.
- [ ] Report reclaimable and live bytes before compaction.
- [ ] Preserve crash recovery and atomic generation switching during maintenance.

### Backup, recovery, and upgrades

- [ ] Add a native snapshot command with checksums and an integrity manifest.
- [ ] Add restore verification and a non-destructive recovery rehearsal command.
- [ ] Add index inspection, validation, rebuild, and repair commands.
- [ ] Version every persisted format and document supported upgrade paths.
- [ ] Add migration and rollback tooling before changing a stable format.
- [ ] Implement graceful shutdown that drains active work and checkpoints ingestion.

### Observability and administration

- [ ] Add a metrics endpoint for QPS, latency distributions, queue depth, memory, mapped bytes,
  index sizes, cache behavior, ingestion progress, embedding latency, and provider failures.
- [ ] Add structured logs with request/trace correlation and configurable rotation.
- [ ] Add a `doctor` command for configuration, storage, model, accelerator, and permissions checks.
- [ ] Add capacity warnings for disk, memory, index growth, and pending ingestion.
- [ ] Publish baseline dashboards and alert recommendations.

### Security and tenancy

- [ ] Replace shared bearer-key semantics with key identities and explicit read/write permissions.
- [ ] Bind credentials to allowed tenants and access scopes.
- [ ] Support multiple scopes or ACL lists per document and request.
- [ ] Add independent per-tenant query, ingestion, storage, and provider quotas.
- [ ] Add auditable administrative actions and key rotation workflows.

## V1.2: semantic document ingestion

Improve retrieval quality where fixed-size chunking separates information from the context needed
to interpret it. Keep the deterministic native chunker as the fast, reproducible default; use an
LLM selectively only where it produces a measurable relevance improvement.

### Normalized document structure

- [ ] Define a native document model for titles, headings, paragraphs, lists, tables, code blocks,
  captions, footnotes, citations, and source offsets.
- [ ] Add format adapters incrementally, starting with Markdown, HTML, and plain text.
- [ ] Preserve section hierarchy and adjacency through existing document graph nodes and edges.
- [ ] Keep byte, page, and section offsets needed to reconstruct faithful citations.
- [ ] Prevent headings, table labels, definitions, and qualifiers from becoming orphaned.

### Adaptive chunking

- [ ] Split on structural and semantic boundaries before applying token-size limits.
- [ ] Add overlap only where boundary analysis indicates that context crosses chunks.
- [ ] Attach undersized fragments to the appropriate parent or neighbor.
- [ ] Detect oversized or dense sections requiring semantic subdivision.
- [ ] Record chunking strategy, parser version, and source span for reproducibility.

### Optional LLM-assisted analysis

- [ ] Add a provider-independent ingestion-analysis interface using existing custom model URLs and
  environment-based API keys.
- [ ] Use an LLM only for ambiguous boundaries, dense prose, tables, and mixed-topic sections.
- [ ] Support local inference with GPU acceleration and CPU fallback.
- [ ] Generate contextual chunk headers or retrieval summaries without replacing source text.
- [ ] Require validated structured output, timeouts, retry limits, and deterministic fallback.
- [ ] Prevent document prompt injection from changing ingestion policy or accessing credentials.
- [ ] Record model identity, prompt version, decisions, latency, and token usage.

### Semantic-ingestion operations

- [ ] Add `fast`, `structured`, and `semantic` modes with explicit resource budgets.
- [ ] Support background analysis, cancellation, progress, and restart-safe checkpoints.
- [ ] Cache analysis by content hash and configuration.
- [ ] Bound per-document cost, time, output size, graph fan-out, and generated metadata.
- [ ] Reindex safely when parser, chunker, prompt, or embedding identity changes.

## V1.3: retrieval quality

- [ ] Add an optional lightweight reranker over only the final shortlist.
- [ ] Classify queries to select lexical, vector, hybrid, or graph-heavy execution.
- [ ] Support bounded query rewriting for abbreviations, domain terminology, and follow-ups.
- [ ] Add metadata set, range, and compound predicates to RAG retrieval.
- [ ] Add configurable recency, authority, and source-quality weighting.
- [ ] Detect duplicate and near-duplicate content during ingestion.
- [ ] Improve confidence estimation and insufficient-evidence decisions.
- [ ] Verify that generated claims are supported by their cited passages.
- [ ] Add relevance-feedback endpoints and offline tuning from accepted feedback.
- [ ] Keep graph expansion and reranking out of requests where they do not improve results.

### Retrieval acceptance criteria

- [ ] Build a real-document evaluation set covering prose, manuals, policies, tables, code, and
  cross-section questions.
- [ ] Compare fixed, structural, and semantic chunking with Recall@k, MRR/nDCG, answer correctness,
  citation correctness, and insufficient-evidence behavior.
- [ ] Measure ingestion throughput, query latency, peak memory, index size, and provider cost.
- [ ] Retain optional processing only for document classes where it produces a repeatable win.

## V1.4: ingestion ecosystem

- [ ] Add PDF, DOCX, HTML, email, JSON, Markdown, and source-code adapters.
- [ ] Add OCR integration for scanned sources while preserving page coordinates.
- [ ] Preserve table headers, rows, captions, and references as structured content.
- [ ] Add directory watching and incremental source synchronization.
- [ ] Add sitemap and authenticated web ingestion.
- [ ] Add connectors for S3-compatible storage, Git repositories, Google Drive, and SharePoint.
- [ ] Detect changed, unchanged, renamed, and deleted source documents.
- [ ] Add content-hash deduplication before embedding.
- [ ] Add language detection and model routing for multilingual corpora.
- [ ] Produce an ingestion manifest containing provenance, successes, warnings, and rejections.

Connectors should feed the normalized document representation and remain outside the core query
engine. No connector should become a mandatory runtime dependency.

## V1.5: agent and developer experience

- [ ] Publish an OpenAPI specification tested against the implemented HTTP routes.
- [ ] Add a native C++ client plus lightweight Go, Rust, JavaScript, and Python clients.
- [ ] Add an MCP server exposing retrieval, graph expansion, ingestion, and trace inspection.
- [ ] Publish portable function schemas for non-MCP agent runtimes.
- [ ] Add bounded conversation-aware retrieval sessions.
- [ ] Add a corpus, ingestion, trace, and relevance inspection interface.
- [ ] Add one-command initialization and persistent hardware calibration.
- [ ] Add instance export/import and migration tools.
- [ ] Maintain compatibility tests for OpenAI clients, OpenClaw, Hermes, and other agent hosts.

## V2 candidates: scale and availability

These are larger architectural projects and should be driven by demonstrated demand rather than
delay the single-node V1.x roadmap.

- [ ] Read replicas and replicated snapshots.
- [ ] Automatic failover and promotion.
- [ ] Sharding by tenant or vector space.
- [ ] Distributed ingestion workers.
- [ ] Rolling upgrades across compatible nodes.
- [ ] Multi-node query routing and bounded result fusion.
- [ ] Tiered hot, warm, and cold content/index storage.
- [ ] Object-storage-backed snapshots.
- [ ] Federated retrieval across independently administered MimicRAG instances.

## Current recommended order

1. Document deletion, retention, and compaction.
2. Snapshot/restore verification and repair tooling.
3. Metrics, diagnostics, and graceful shutdown.
4. Credential-bound tenant ACLs and quotas.
5. Semantic document ingestion.
6. Optional shortlist reranking and citation verification.
7. MCP integration and stable API specifications.
8. High-value document adapters and connectors.

## Non-goals

- Replacing source documents with generated summaries.
- Sending every document to an LLM unconditionally.
- Trusting generated boundaries or metadata without validation.
- Requiring a distributed deployment for ordinary installations.
- Trading predictable behavior for benchmark-only optimizations.
