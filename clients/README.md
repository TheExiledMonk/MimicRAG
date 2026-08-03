# MimicRAG lightweight clients

This directory contains dependency-light C++, Go, Rust, and JavaScript clients. The installable
Python client is `mimicrag_dev.Client`. All clients preserve native JSON responses and support
Bearer authentication. Treat tenant and access scope as trusted application configuration rather
than model-controlled input.

## Dependencies

- C++: libcurl and nlohmann-json; include `cpp/mimicrag_client.h` and initialize libcurl once in the
  application process when required by your libcurl build.
- Go: standard library only; place `go/mimicrag.go` in your module under package `mimicrag`.
- Rust: use `rust` as a path dependency; it uses `ureq` with JSON support and `serde_json`.
- JavaScript: an ES module runtime with the standard `fetch` API.
- Python: `python -m pip install -e ./api`, then `from mimicrag_dev import Client`.

Example Python request:

```python
from mimicrag_dev import Client

client = Client("https://rag.example.com", api_key="...")
result = client.retrieve("deployment requirements", tenant_id="docs", top_k=5)
```

These clients are deliberately small. Applications own retries, observability, credential refresh,
tenant/scope policy, and generated response types. The authoritative route contract is
[`../docs/openapi.json`](../docs/openapi.json).
