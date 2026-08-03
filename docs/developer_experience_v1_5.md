# MimicRAG V1.5 developer experience

V1.5 publishes contracts and thin clients around the existing native HTTP API. The complete,
reviewable contract is [`openapi.json`](openapi.json). The native `GET /openapi.json` endpoint is a
runtime route summary generated in C++; it is intentionally smaller than the checked-in contract.
Portable non-MCP agent definitions live in [`function-schemas.json`](function-schemas.json).

## Python installation and client

```bash
python -m pip install -e ./api
```

```python
from mimicrag_dev import Client, RetrievalSession

client = Client("https://rag.example.com", api_key="...", timeout=30)
hits = client.retrieve("What is the retention policy?", tenant_id="acme", top_k=5)
answer = client.answer("Summarize the policy", tenant_id="acme")

session = RetrievalSession(
    client,
    tenant_id="acme",
    maximum_turns=6,
    maximum_context_chars=8_000,
)
follow_up = session.ask("Does it apply to backups?")
```

`RetrievalSession` is client-side bounded context, not a server session. It trims old messages by
turn and character limits and sends the resulting conversation with the next answer request.

## Developer and inspection CLI

The CLI reads `MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY`, or accepts `--server` and `--api-key`:

```bash
python -m mimicrag_dev init ./instance
python -m mimicrag_dev inspect storage
python -m mimicrag_dev inspect corpus "retention policy" --limit 20
python -m mimicrag_dev inspect trace TRACE_ID
python -m mimicrag_dev inspect manifest ingestion.json
python -m mimicrag_dev export ./snapshot --binary ./build-release/rag_cpp/mimicrag_server
python -m mimicrag_dev import ./snapshot --binary ./build-release/rag_cpp/mimicrag_server
python -m mimicrag_dev migrate --binary ./build-release/rag_cpp/mimicrag_server
```

`init` creates a data directory and a minimal configuration with a persisted worker count based on
logical CPUs. It does not download an embedding model, generate credentials, or install a service.
Review the generated configuration before serving. `export`, `import`, and `migrate` delegate to
the native checksum-verifying commands and inherit their stopped-writer/rehearsal requirements.

## MCP server

The MCP server uses newline-delimited JSON-RPC over stdio and exposes retrieval, graph expansion,
approved ingestion, and trace inspection. Example host configuration:

```json
{
  "mcpServers": {
    "mimicrag": {
      "command": "python",
      "args": ["-m", "mimicrag_dev.mcp_server"],
      "env": {
        "MIMICRAG_BASE_URL": "https://rag.example.com",
        "MIMICRAG_API_KEY": "set-through-the-host-secret-store"
      }
    }
  }
}
```

The MCP process does not enforce an approval UI. Give ordinary research agents read-only API keys;
expose the ingestion tool only in a host that applies an explicit approval boundary. Tenant and
scope should come from trusted host configuration rather than unconstrained model arguments.

## Lightweight clients

The [`clients/`](../clients/) directory contains source-only clients:

- C++: include `clients/cpp/mimicrag_client.h`; link libcurl and provide nlohmann-json headers.
- Go: add `clients/go/mimicrag.go` to a module or copy its small package into your client project.
- Rust: use `clients/rust` as a path dependency; it depends on `ureq` and `serde_json`.
- JavaScript: import `MimicRagClient` from `clients/javascript/index.js` in a runtime with `fetch`.
- Python: use the installed `mimicrag_dev.Client`.

These are intentionally narrow HTTP wrappers. They preserve server JSON and do not implement
automatic retries, pagination, credential discovery, tenant policy, or generated typed models.

## Contracts and compatibility

OpenAI-compatible hosts use `/v1/chat/completions`. Controlled tool hosts can use MCP or
[`function-schemas.json`](function-schemas.json). The compatibility tests assert route/client/tool
surfaces and request shapes; they are not certifications for every third-party agent version.

When changing a route, update all of the following together:

1. `rag_cpp/src/http_server.cpp`
2. `docs/openapi.json` and the runtime `OpenApiSpec()` summary
3. affected clients and portable function schemas
4. `tests/test_mimicrag_v15.py`
