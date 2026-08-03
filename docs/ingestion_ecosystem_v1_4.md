# MimicRAG V1.4 ingestion ecosystem

The `mimicrag_ingestion` Python package is an optional source-side companion to the native
MimicRAG server. It converts every connector result into a normalized document and posts that
representation to `/v1/documents`; connector SDKs are never linked into the query engine.

Built-in adapters cover text, Markdown, source code, HTML, JSON, RFC 822 email, PDF, and DOCX.
PDF and DOCX support load `pypdf` and `python-docx` only when those formats are used. An OCR
callback can return text plus `(x1, y1, x2, y2)` page coordinates. Tables retain headers, rows,
captions, and references in `metadata.structured_content`.

Sources include directories, authenticated sitemaps/web pages, Git, S3-compatible stores, Google
Drive, and SharePoint. Cloud connectors either load an optional SDK or accept an already
authenticated client. Each sync writes an atomic provenance manifest recording creates, changes,
unchanged files, renames, deletions, content-hash duplicates, warnings, and rejections. Language
detection selects a configured embedding/analysis model route.

Examples:

```bash
PYTHONPATH=api python -m mimicrag_ingestion directory ./manuals --state ./ingestion.json
PYTHONPATH=api python -m mimicrag_ingestion sitemap https://example.com/sitemap.xml --bearer-token "$TOKEN"
PYTHONPATH=api python -m mimicrag_ingestion git https://github.com/acme/manuals --ref main
PYTHONPATH=api python -m mimicrag_ingestion s3 docs --endpoint-url https://objects.example.com --prefix current/
```

Add `--watch` to poll a directory or remote source and incrementally synchronize it. The manifest
is also the durable synchronization state, so retain it between runs.
