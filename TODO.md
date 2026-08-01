# MimicDB / MimicRAG roadmap

## Next feature release: semantic document ingestion

Improve retrieval quality where fixed-size chunking separates information from the context needed
to interpret it. Keep the existing deterministic native chunker as the fast, reproducible default;
use an LLM selectively where it produces a measurable relevance improvement.

### Document structure

- [ ] Define a normalized native document model for titles, headings, paragraphs, lists, tables,
  code blocks, captions, footnotes, citations, and source offsets.
- [ ] Add format adapters incrementally, starting with Markdown, HTML, and plain text.
- [ ] Preserve section hierarchy and adjacency through existing document graph nodes and edges.
- [ ] Keep byte/page/section offsets needed to reconstruct faithful citations from the source.
- [ ] Prevent headings, table labels, definitions, and qualifiers from being orphaned from content.

### Adaptive chunking

- [ ] Split on structural and semantic boundaries before applying token-size limits.
- [ ] Add overlap only where boundary analysis indicates that context crosses chunks.
- [ ] Detect undersized fragments that should remain attached to their parent or neighbor.
- [ ] Detect oversized or dense sections that need semantic subdivision.
- [ ] Store chunking strategy, parser version, and source span in metadata for reproducibility.

### Optional LLM-assisted analysis

- [ ] Add a provider-independent ingestion-analysis interface using the existing model configuration
  and custom URL/API-key support.
- [ ] Use the LLM only for ambiguous boundaries, dense prose, tables, mixed topics, and other
  configured cases; do not require it for ordinary ingestion.
- [ ] Support local inference with GPU acceleration and CPU fallback.
- [ ] Generate contextual chunk headers or short retrieval summaries without replacing source text.
- [ ] Require structured output with strict validation, timeouts, retry limits, and deterministic
  fallback to the native chunker.
- [ ] Treat document contents as untrusted data and prevent indexed prompt injection from changing
  ingestion policy or accessing credentials.
- [ ] Record model identity, prompt version, decisions, latency, and token usage for auditing.

### Retrieval integration

- [ ] Evaluate embedding the original chunk, contextualized chunk, or both without mixing
  incompatible vector spaces.
- [ ] Use parent, sibling, definition, table, and continuation edges for bounded context recovery.
- [ ] Load expanded source text only after retrieval winners are selected.
- [ ] Allow graph deep-dives to return the complete logical section while preserving precise chunk
  citations.
- [ ] Reindex documents safely when the parser, chunker, prompt, or embedding model changes.

### Operations and controls

- [ ] Add `fast`, `structured`, and `semantic` ingestion modes with explicit resource budgets.
- [ ] Support background analysis jobs, cancellation, progress, and restart-safe checkpoints.
- [ ] Cache analysis by content hash and analysis configuration to avoid repeated LLM work.
- [ ] Bound per-document cost, time, output size, graph fan-out, and generated metadata.
- [ ] Keep tenant and access-scope metadata attached throughout parsing and background processing.

### Benchmarks and release criteria

- [ ] Build a real-document evaluation set containing prose, manuals, policies, tables, code, and
  cross-paragraph questions.
- [ ] Compare fixed, structural, and LLM-assisted chunking using Recall@k, MRR/nDCG, answer
  correctness, citation correctness, and insufficient-evidence behavior.
- [ ] Measure ingestion throughput, query latency, peak memory, index size, and provider cost.
- [ ] Retain LLM-assisted processing only for document classes where it produces a repeatable win.
- [ ] Require no meaningful regression in the existing fast retrieval path.
- [ ] Document migration, rollback, and reproducibility before enabling semantic mode by default.

### Non-goals

- Replacing original documents with generated summaries.
- Sending all documents to an LLM unconditionally.
- Trusting generated boundaries or metadata without validation.
- Increasing query latency when graph expansion or semantic context is not needed.
