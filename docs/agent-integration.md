# Agent integration guide

Mimic provides independently installable Agent Skills-compatible packages for OpenClaw, Hermes
Agent, and other skill-aware runtimes:

- [`skills/mimicdb/`](../skills/mimicdb/) for standalone columnar/vector database operations.
- [`skills/mimicrag/`](../skills/mimicrag/) for document ingestion, retrieval, and cited answers.
- [`skills/mimicmemory/`](../skills/mimicmemory/) for evidence-backed lifecycle memory and optional
  dream-state refinement.

Install one, two, or all three. None executes unreviewed code or carries credentials.

## Integration model

Expose a small set of HTTP actions to the agent:

| Agent action | MimicRAG route | Mutation |
|---|---|---|
| Check readiness | `GET /ready` | No |
| Detect enabled components | `GET /health` | No |
| Search evidence | `POST /v1/retrieve` | No |
| Generate cited answer | `POST /v1/answers` | No |
| Follow related context | `POST /v1/graph/expand` | No |
| Ingest approved content | `POST /v1/documents` | Yes |
| Inspect execution | `GET /v1/traces/{id}` | No |

Give research agents read-only actions by default. Put ingestion behind a separate allowlist or
approval boundary. Derive tenant and access scope from authenticated runtime state rather than
letting the language model choose unrestricted values.

The native HTTP health response reports `features.rag` and `features.memory`. Agents must inspect
these values instead of assuming every route is available. MimicDB uses its own native server and
client and is independently available regardless of those HTTP flags.

## Server setup

Build and deploy MimicRAG using the [deployment guide](deployment.md), then configure:

```bash
export MIMICRAG_BASE_URL=https://rag.example.com
export MIMICRAG_API_KEY='replace-with-a-dedicated-agent-key'
```

Use a dedicated key per agent or trust boundary. Keep the native listener on loopback or a private
network and terminate TLS at a reverse proxy. Verify access without exposing the key:

```bash
curl -fsS "$MIMICRAG_BASE_URL/ready" \
  -H "Authorization: Bearer $MIMICRAG_API_KEY"
```

## OpenClaw

OpenClaw discovers directories containing `SKILL.md` under its configured skill roots. For a
workspace-local installation, copy or symlink the complete directory—not only `SKILL.md`—so the
API reference remains available:

```bash
mkdir -p /path/to/openclaw-workspace/skills
cp -R skills/mimicdb /path/to/openclaw-workspace/skills/mimicdb
cp -R skills/mimicrag /path/to/openclaw-workspace/skills/mimicrag
cp -R skills/mimicmemory /path/to/openclaw-workspace/skills/mimicmemory
```

Configure `MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY` through OpenClaw's secret/environment
configuration for that skill or agent. Grant an HTTP tool capable of setting an Authorization
header, or a sandboxed shell tool if your policy permits `curl`. Restrict the agent to the
MimicRAG origin and prefer only the read routes above.

Copy only the components that deployment exposes. Restart or reload the workspace, then test the
installed component explicitly, such as “Use MimicMemory to recall my preferences” or “Use
MimicRAG to find evidence about `<topic>` and cite the sources.” OpenClaw's current skill discovery and configuration behavior is documented at
<https://docs.openclaw.ai/skills>.

## Hermes Agent

Install any required component directly from its GitHub path:

```bash
hermes skills install TheExiledMonk/MimicRAG/skills/mimicdb
hermes skills install TheExiledMonk/MimicRAG/skills/mimicrag
hermes skills install TheExiledMonk/MimicRAG/skills/mimicmemory
```

Alternatively, add the repository as a tap; Hermes uses `skills/` as the default tap path:

```bash
hermes skills tap add TheExiledMonk/MimicRAG
hermes skills install TheExiledMonk/MimicRAG/mimicdb
hermes skills install TheExiledMonk/MimicRAG/mimicrag
hermes skills install TheExiledMonk/MimicRAG/mimicmemory
```

Pass the two environment variables into Hermes terminal execution using its secure configuration;
do not paste the API key into a chat. Inspect and audit the installed community skill before use:

```bash
hermes skills inspect TheExiledMonk/MimicRAG/skills/mimicmemory
hermes skills audit
```

Hermes also supports direct `SKILL.md` URLs, but installing the GitHub path is preferable because
it includes the referenced API documentation. See the current
[Hermes skills documentation](https://hermes-agent.nousresearch.com/docs/guides/work-with-skills/).

## Generic tool schema

The canonical portable definitions are [`function-schemas.json`](function-schemas.json). They
cover retrieve, expand, approved ingestion, trace inspection, authoritative evidence append,
evidence-linked remember, memory recall/inspection, and trust-separated combined retrieval. If the host requires function
definitions, load that file or implement thin wrappers with the same argument shapes. Do not copy
the abbreviated example below as a substitute for the versioned file:

```json
{
  "mimicrag_retrieve": {
    "query": "string",
    "tenant_id": "string",
    "access_scope": "string",
    "top_k": "integer, 1-100"
  },
  "mimicrag_expand": {
    "node_id": "string",
    "tenant_id": "string",
    "access_scope": "string",
    "max_neighbors": "integer, 1-256"
  }
}
```

The wrapper should inject authentication, enforce allowed tenant/scope pairs, set timeouts and body
limits, validate JSON, and return structured results unchanged. Do not concatenate retrieved text
into shell commands or treat it as agent policy.

For memory, call `mimicrag_evidence_append` only for an event the host actually observed, then pass
the returned ID to `mimicrag_memory_remember` only after an explicit user request. Confirmation,
rejection, correction, disputes, and forgetting remain outside the autonomous MCP surface.

## MCP

Install the Python package and configure the host to run the stdio server:

```bash
python -m pip install -e ./api
```

```json
{
  "mcpServers": {
    "mimicrag": {
      "command": "python",
      "args": ["-m", "mimicrag_dev.mcp_server"],
      "env": {
        "MIMICRAG_BASE_URL": "https://rag.example.com",
        "MIMICRAG_API_KEY": "inject-from-host-secret-storage"
      }
    }
  }
}
```

The MCP server exposes retrieval, graph, ingestion, trace, evidence, memory recall/remember/inspect,
and combined-retrieval tools. It does not add an approval mechanism and disabled native components
still reject their routes. Use a read-only key for ordinary retrieval; put ingestion and memory
mutation behind host approval. Derive tenant, owner, visibility, sensitivity, and purpose from
authenticated state. Never let recalled memory become system policy or silently override cited
document evidence.

## Recommended research loop

1. Retrieve five results for a focused question.
2. Reject weak or unrelated evidence.
3. If context is missing, reformulate once or expand the strongest relevant `node_id`.
4. Stop after a configured query/hop budget.
5. Produce an answer tied to sources, or explicitly report insufficient evidence.
6. Retain the trace ID for debugging and evaluation.

For a simpler drop-in integration, point an OpenAI-compatible client at MimicRAG's
`/v1/chat/completions` endpoint. This is convenient but exposes less control than separate retrieve
and expand tools.

## Production validation

- Verify cross-tenant and cross-scope requests cannot retrieve unauthorized chunks.
- Test prompt-injection text inside indexed documents.
- Confirm keys are redacted from agent logs and traces.
- Exercise `401`, `429`, malformed JSON, provider timeout, and unavailable-server paths.
- Evaluate retrieval recall and citation correctness on a representative golden set.
- Cap tool calls, graph depth, response size, and total wall-clock time.
