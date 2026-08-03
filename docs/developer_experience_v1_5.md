# MimicRAG V1.5 developer experience

The canonical API contract is [`openapi.json`](openapi.json), also served by the native process at
`GET /openapi.json`. Portable agent definitions live in [`function-schemas.json`](function-schemas.json).

The installable `mimicrag_dev` package provides a dependency-free Python client, bounded
conversation-aware retrieval sessions, an MCP stdio server, and operational tooling:

```bash
PYTHONPATH=api python -m mimicrag_dev init ./instance
PYTHONPATH=api python -m mimicrag_dev inspect storage
PYTHONPATH=api python -m mimicrag_dev inspect corpus "retention policy" --limit 20
PYTHONPATH=api python -m mimicrag_dev inspect trace TRACE_ID
PYTHONPATH=api python -m mimicrag_dev inspect manifest ingestion.json
PYTHONPATH=api python -m mimicrag_dev export ./snapshot --binary ./build/rag_cpp/mimicrag_server
PYTHONPATH=api python -m mimicrag_dev import ./snapshot --binary ./build/rag_cpp/mimicrag_server
```

Configure an MCP host to execute `python -m mimicrag_dev.mcp_server`, passing
`MIMICRAG_BASE_URL` and `MIMICRAG_API_KEY` through its secret environment. Available tools cover
retrieval, graph expansion, ingestion, and trace inspection. The `clients/` directory contains
small C++, Go, Rust, and JavaScript clients; Python applications use `mimicrag_dev.Client`.

Snapshot import/export and format migration delegate to the native checksum-verifying operational
commands. OpenAI-compatible hosts use `/v1/chat/completions`; controlled tool hosts can use MCP or
the portable function schemas. Compatibility tests assert all published surfaces remain present.
