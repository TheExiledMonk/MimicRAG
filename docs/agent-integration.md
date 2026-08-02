# Agent integration guide

MimicRAG can serve as a shared evidence service for OpenClaw, Hermes Agent, and any runtime that
can call an HTTP API. The published agent skill lives in [`skills/mimicrag/`](../skills/mimicrag/).
It is an Agent Skills-compatible instruction package; it does not execute unreviewed code or carry
credentials.

## Integration model

Expose a small set of HTTP actions to the agent:

| Agent action | MimicRAG route | Mutation |
|---|---|---|
| Check readiness | `GET /ready` | No |
| Search evidence | `POST /v1/retrieve` | No |
| Generate cited answer | `POST /v1/answers` | No |
| Follow related context | `POST /v1/graph/expand` | No |
| Ingest approved content | `POST /v1/documents` | Yes |
| Inspect execution | `GET /v1/traces/{id}` | No |

Give research agents read-only actions by default. Put ingestion behind a separate allowlist or
approval boundary. Derive tenant and access scope from authenticated runtime state rather than
letting the language model choose unrestricted values.

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
cp -R skills/mimicrag /path/to/openclaw-workspace/skills/mimicrag
```

Configure `MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY` through OpenClaw's secret/environment
configuration for that skill or agent. Grant an HTTP tool capable of setting an Authorization
header, or a sandboxed shell tool if your policy permits `curl`. Restrict the agent to the
MimicRAG origin and prefer only the read routes above.

Restart or reload the workspace, then test with: “Use MimicRAG to find evidence about `<topic>` and
cite the sources.” OpenClaw's current skill discovery and configuration behavior is documented at
<https://docs.openclaw.ai/skills>.

## Hermes Agent

Once this repository is public, install the skill directly from its GitHub path:

```bash
hermes skills install TheExiledMonk/MimicRAG/skills/mimicrag
```

Alternatively, add the repository as a tap; Hermes uses `skills/` as the default tap path:

```bash
hermes skills tap add TheExiledMonk/MimicRAG
hermes skills install TheExiledMonk/MimicRAG/mimicrag
```

Pass the two environment variables into Hermes terminal execution using its secure configuration;
do not paste the API key into a chat. Inspect and audit the installed community skill before use:

```bash
hermes skills inspect TheExiledMonk/MimicRAG/skills/mimicrag
hermes skills audit
```

Hermes also supports direct `SKILL.md` URLs, but installing the GitHub path is preferable because
it includes the referenced API documentation. See the current
[Hermes skills documentation](https://hermes-agent.nousresearch.com/docs/guides/work-with-skills/).

## Generic tool schema

If the host requires function definitions, implement thin wrappers with these argument shapes:

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
