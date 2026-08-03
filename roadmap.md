# MimicDB / MimicRAG / MimicMemory roadmap

This roadmap begins after the V1.0 release. Priorities may change based on production evidence, but
new work should preserve the core proposition: one fast native package, predictable resource use,
hardware-aware execution, and no mandatory external retrieval services.

The long-term product direction is a unified evidence and memory engine: MimicRAG ingests and
retrieves external knowledge, while MimicMemory forms, maintains, recalls, and forgets agent
experience using the same fast indexes, predicates, graph, persistence, and native server.

## Engineering principles

- Keep the single-node, single-package path first-class as distributed features are added.
- Preserve deterministic CPU fallbacks for optional GPU and model-assisted features.
- Measure end-to-end retrieval, not isolated kernels alone.
- Retain optimizations only when representative benchmarks show a repeatable improvement.
- Keep source text and provenance authoritative; generated metadata is advisory.
- Treat language models as proposal engines, never unrestricted storage authorities.
- Apply authorization filters before loading or scoring protected content.
- Keep agent policy, user memory, project memory, and imported knowledge in separate trust domains.
- Avoid meaningful regressions in the existing fast retrieval path.

## V1.1: operational maturity

### Data lifecycle

- [x] Add document deletion and replacement APIs with removal from vector, lexical, graph, content,
  and trace references.
- [x] Support retention policies and verifiable tenant data erasure.
- [x] Add online compaction for superseded versions, tombstones, content storage, and indexes.
- [x] Report reclaimable and live bytes before compaction.
- [x] Preserve crash recovery and atomic generation switching during maintenance.

### Backup, recovery, and upgrades

- [x] Add a native snapshot command with checksums and an integrity manifest.
- [x] Add restore verification and a non-destructive recovery rehearsal command.
- [x] Add index inspection, validation, rebuild, and repair commands.
- [x] Version every persisted format and document supported upgrade paths.
- [x] Add migration and rollback tooling before changing a stable format.
- [x] Implement graceful shutdown that drains active work and checkpoints ingestion.

### Observability and administration

- [x] Add a metrics endpoint for QPS, latency distributions, queue depth, memory, mapped bytes,
  index sizes, cache behavior, ingestion progress, embedding latency, and provider failures.
- [x] Add structured logs with request/trace correlation and configurable rotation.
- [x] Add a `doctor` command for configuration, storage, model, accelerator, and permissions checks.
- [x] Add capacity warnings for disk, memory, index growth, and pending ingestion.
- [x] Publish baseline dashboards and alert recommendations.

### Security and tenancy

- [x] Replace shared bearer-key semantics with key identities and explicit read/write permissions.
- [x] Bind credentials to allowed tenants and access scopes.
- [x] Support multiple scopes or ACL lists per document and request.
- [x] Add independent per-tenant query, ingestion, storage, and provider quotas.
- [x] Add auditable administrative actions and key rotation workflows.

## V1.2: semantic document ingestion

Improve retrieval quality where fixed-size chunking separates information from the context needed
to interpret it. Keep the deterministic native chunker as the fast, reproducible default; use an
LLM selectively only where it produces a measurable relevance improvement.

### Normalized document structure

- [x] Define a native document model for titles, headings, paragraphs, lists, tables, code blocks,
  captions, footnotes, citations, and source offsets.
- [x] Add format adapters incrementally, starting with Markdown, HTML, and plain text.
- [x] Preserve section hierarchy and adjacency through existing document graph nodes and edges.
- [x] Keep byte, page, and section offsets needed to reconstruct faithful citations.
- [x] Prevent headings, table labels, definitions, and qualifiers from becoming orphaned.

### Adaptive chunking

- [x] Split on structural and semantic boundaries before applying token-size limits.
- [x] Add overlap only where boundary analysis indicates that context crosses chunks.
- [x] Attach undersized fragments to the appropriate parent or neighbor.
- [x] Detect oversized or dense sections requiring semantic subdivision.
- [x] Record chunking strategy, parser version, and source span for reproducibility.

### Optional LLM-assisted analysis

- [x] Add a provider-independent ingestion-analysis interface using existing custom model URLs and
  environment-based API keys.
- [x] Use an LLM only for ambiguous boundaries, dense prose, tables, and mixed-topic sections.
- [x] Support local inference with GPU acceleration and CPU fallback.
- [x] Generate contextual chunk headers or retrieval summaries without replacing source text.
- [x] Require validated structured output, timeouts, retry limits, and deterministic fallback.
- [x] Prevent document prompt injection from changing ingestion policy or accessing credentials.
- [x] Record model identity, prompt version, decisions, latency, and token usage.

### Semantic-ingestion operations

- [x] Add `fast`, `structured`, and `semantic` modes with explicit resource budgets.
- [x] Support background analysis, cancellation, progress, and restart-safe checkpoints.
- [x] Cache analysis by content hash and configuration.
- [x] Bound per-document cost, time, output size, graph fan-out, and generated metadata.
- [x] Reindex safely when parser, chunker, prompt, or embedding identity changes.

## V1.3: retrieval quality

- [x] Add an optional lightweight reranker over only the final shortlist.
- [x] Classify queries to select lexical, vector, hybrid, or graph-heavy execution.
- [x] Support bounded query rewriting for abbreviations, domain terminology, and follow-ups.
- [x] Add metadata set, range, and compound predicates to RAG retrieval.
- [x] Add configurable recency, authority, and source-quality weighting.
- [x] Detect duplicate and near-duplicate content during ingestion.
- [x] Improve confidence estimation and insufficient-evidence decisions.
- [x] Verify that generated claims are supported by their cited passages.
- [x] Add relevance-feedback endpoints and offline tuning from accepted feedback.
- [x] Keep graph expansion and reranking out of requests where they do not improve results.

### Retrieval acceptance criteria

- [x] Build a real-document evaluation set covering prose, manuals, policies, tables, code, and
  cross-section questions.
- [x] Compare fixed, structural, and semantic chunking with Recall@k, MRR/nDCG, answer correctness,
  citation correctness, and insufficient-evidence behavior.
- [x] Measure ingestion throughput, query latency, peak memory, index size, and provider cost.
- [x] Retain optional processing only for document classes where it produces a repeatable win.

## V1.4: ingestion ecosystem

- [x] Add PDF, DOCX, HTML, email, JSON, Markdown, and source-code adapters.
- [x] Add OCR integration for scanned sources while preserving page coordinates.
- [x] Preserve table headers, rows, captions, and references as structured content.
- [x] Add directory watching and incremental source synchronization.
- [x] Add sitemap and authenticated web ingestion.
- [x] Add connectors for S3-compatible storage, Git repositories, Google Drive, and SharePoint.
- [x] Detect changed, unchanged, renamed, and deleted source documents.
- [x] Add content-hash deduplication before embedding.
- [x] Add language detection and model routing for multilingual corpora.
- [x] Produce an ingestion manifest containing provenance, successes, warnings, and rejections.

Connectors should feed the normalized document representation and remain outside the core query
engine. No connector should become a mandatory runtime dependency.

## V1.5: agent and developer experience

- [x] Publish an OpenAPI specification tested against the implemented HTTP routes.
- [x] Add a native C++ client plus lightweight Go, Rust, JavaScript, and Python clients.
- [x] Add an MCP server exposing retrieval, graph expansion, ingestion, and trace inspection.
- [x] Publish portable function schemas for non-MCP agent runtimes.
- [x] Add bounded conversation-aware retrieval sessions.
- [x] Add a corpus, ingestion, trace, and relevance inspection interface.
- [x] Add one-command initialization and persistent hardware calibration.
- [x] Add instance export/import and migration tools.
- [x] Maintain compatibility tests for OpenAI clients, OpenClaw, Hermes, and other agent hosts.

## V1.6: unified agent memory

Add short- and long-term memory without creating another service. Reuse MimicDB storage and
MimicRAG retrieval, but keep memory namespaces, lifecycle rules, trust, and weighting distinct from
imported documents.

### Memory model and evidence

- [ ] Add explicit namespaces for working, episodic, semantic, procedural, preference,
  prospective, and negative memory.
- [ ] Store an immutable event/evidence log for conversations, tool results, corrections, and task
  outcomes before forming durable memories.
- [ ] Define a versioned memory record containing subject, canonical statement, type, event time,
  validity interval, confidence, importance, provenance, reinforcement, decay policy, visibility,
  and lifecycle status.
- [ ] Link every inferred memory to supporting evidence and optional contradicting evidence.
- [ ] Keep raw evidence authoritative and make every consolidation reversible.
- [ ] Never allow imported documents or inferred memories to modify agent identity or policy.

### Short-term and working memory

- [ ] Add a compact mutable working-memory state for the current objective, active entities,
  constraints, assumptions, unresolved questions, recent observations, and pending operations.
- [ ] Add session and task boundaries with configurable expiration.
- [ ] Promote only selected outcomes from working memory into durable memory.
- [ ] Build bounded context packets instead of replaying complete conversation histories.
- [ ] Persist crash-safe task state without making temporary observations permanently prominent.

### Long-term memory and associations

- [ ] Add typed, weighted, timestamped graph relations such as `supports`, `contradicts`,
  `supersedes`, `caused`, `resolved_by`, `preference_of`, and `remind_when`.
- [ ] Add temporal reasoning across event time, recording time, validity, and supersession.
- [ ] Reinforce memories through repeated independent evidence and successful use.
- [ ] Decay retrieval priority for unused episodic memories without immediately deleting evidence.
- [ ] Preserve superseded memories for historical questions while excluding them from current-fact
  retrieval.
- [ ] Add prospective memories activated by bounded time, entity, state, or action conditions.

### Explicit memory API

- [ ] Add native `remember`, `recall`, `correct`, `forget`, `inspect`, and `export` operations.
- [ ] Require tenant, owner, visibility, sensitivity, and allowed-purpose metadata.
- [ ] Add confirmation controls for identity-level, sensitive, legal, financial, and personal
  memories.
- [ ] Make explicit user corrections outrank inferred memories.
- [ ] Implement verifiable forgetting across source events where allowed, derived memories,
  embeddings, lexical indexes, graph edges, caches, traces, backups, and compaction.
- [ ] Add an audit view explaining why a memory was formed, changed, recalled, or suppressed.

### Remote LLM memory manager

- [ ] Add a separate provider-independent `memory_model` configuration that can reuse the existing
  remote chat connection, use a cheaper specialist model, or run locally.
- [ ] Default memory extraction and consolidation to the provider's lowest reliable temperature,
  normally `0.0`; leave `top_p` unset unless provider-specific evaluation proves a benefit.
- [ ] Require strict versioned structured output for every proposed memory operation.
- [ ] Require existing evidence IDs and verify quoted supporting spans before accepting proposals.
- [ ] Validate types, confidence bounds, authorization, provenance, relationships, and operation
  limits in native code.
- [ ] Allow the model to propose creates, reinforcements, associations, supersessions, disputes,
  reminders, and deletions; never allow it to mutate storage directly.
- [ ] Quarantine malformed, unsupported, low-confidence, contradictory, or sensitive proposals for
  rejection or confirmation.
- [ ] Record provider, model identity, prompt/schema version, input evidence IDs, output, latency,
  token usage, validation result, and final mutation IDs.
- [ ] Keep explicit memory operations and retrieval synchronous; run extraction, consolidation,
  reflection, association discovery, and decay asynchronously.
- [ ] Ensure remote-model failure never blocks ordinary RAG retrieval or agent responses.

### Memory formation and consolidation

- [ ] Extract candidate facts, preferences, decisions, commitments, corrections, procedures,
  failures, relationships, and unresolved work at task or session boundaries.
- [ ] Deduplicate candidates against active and superseded memories before writing.
- [ ] Consolidate repeated episodes into semantic or procedural memory while retaining evidence.
- [ ] Distinguish true updates from contextual differences and unresolved contradictions.
- [ ] Add bounded reflection jobs that propose lessons, changed facts, reusable procedures, and
  expired working state.
- [ ] Cache processing by evidence hash, model identity, prompt version, and schema version.
- [ ] Set per-session limits for remote calls, tokens, accepted memories, associations, and
  reflection depth.

### Context-dependent recall

- [ ] Rank memory using semantic and lexical relevance, confidence, importance, reinforcement,
  temporal validity, decay, trust, and current-goal relevance.
- [ ] Select different memory mixes for coding, planning, conversation, research, and sensitive
  actions.
- [ ] Return compact sections for current state, durable facts, relevant episodes, applicable
  procedures, preferences, commitments, uncertainty, and provenance.
- [ ] Support combined RAG and memory retrieval without allowing lower-trust memories to displace
  authoritative document evidence silently.
- [ ] Add query and hop budgets to prevent uncontrolled associative recall loops.

### Privacy and remote-processing controls

- [ ] Make remote memory processing opt-in per tenant and sensitivity class.
- [ ] Add local-only memory classifications and a local-model fallback.
- [ ] Redact configured secrets and sensitive fields before remote processing.
- [ ] Record exactly which evidence was transmitted to which provider and under which policy.
- [ ] Support provider retention, residency, batching, timeout, and maximum-content policies.
- [ ] Prevent credentials, hidden policy, unrelated tenant data, and unauthorized memories from
  entering prompts or model-visible tool output.

### Memory evaluation and acceptance criteria

- [ ] Build long-running conversation and task scenarios covering correct recall, omission,
  correction, contradiction, temporal updates, procedural learning, and prospective reminders.
- [ ] Measure useful recall, missed recall, intrusive recall, stale recall, harmful recall,
  cross-tenant leakage, deletion correctness, latency, token overhead, and remote cost.
- [ ] Test prompt injection attempting to create policy, credential, identity, or cross-tenant
  memories.
- [ ] Verify deterministic fallback when the memory model is unavailable or produces invalid JSON.
- [ ] Require memory processing to remain outside the latency-critical retrieval path unless the
  user explicitly invokes a synchronous memory operation.
- [ ] Require no meaningful regression in existing RAG throughput, startup, or ready-state memory.

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
7. Memory evidence log, namespaces, lifecycle, and explicit memory APIs.
8. Context-dependent memory retrieval and temporal contradiction handling.
9. Evidence-bound remote LLM extraction and asynchronous consolidation.
10. MCP integration and stable API specifications.
11. High-value document adapters and connectors.

## Non-goals

- Replacing source documents with generated summaries.
- Sending every document to an LLM unconditionally.
- Storing every conversation turn as a permanent high-priority memory.
- Allowing an LLM to write, delete, or promote memory without native validation.
- Treating inferred memory as equivalent to authoritative evidence.
- Trusting generated boundaries or metadata without validation.
- Requiring a distributed deployment for ordinary installations.
- Trading predictable behavior for benchmark-only optimizations.
