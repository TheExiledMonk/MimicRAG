# MimicRAG V1.4 ingestion ecosystem

`mimicrag_ingestion` is an optional source-side Python companion to the native server. Connectors
produce a normalized document and submit it to `/v1/documents`; connector SDKs are never imported
or linked by the native query engine.

## Installation

```bash
python -m pip install -e ./api
python -m pip install pypdf python-docx  # only for PDF/DOCX
python -m pip install boto3              # only for S3-compatible storage
```

Text, Markdown, HTML, JSON, RFC 822 `.eml`, and source-code adapters use the standard library.
PDF requires `pypdf`; DOCX requires `python-docx`. Unsupported or malformed documents become
`rejected` manifest entries instead of aborting the complete synchronization.

## Normalized representation

Every adapter produces `NormalizedDocument(source_uri, format, text, title, blocks, metadata,
warnings)`. Blocks may contain a type, page, section path, coordinates, table headers and rows,
caption, and references. The content hash is SHA-256 over whitespace-normalized text and is checked
before calling the embedding endpoint.

The sink sends `text`, `markdown`, or `html`, the formats understood by the native parser. Original
formats and structured blocks are retained under `metadata.source_format` and
`metadata.structured_content`. Language detection records an embedding/analysis route in metadata.
The current native server still uses instance-level embedding providers; this per-document route
does not switch the native model unless an external orchestrator acts on it.

## OCR callback

OCR is invoked only for PDF pages with no extractable text. The callback receives the original PDF
bytes and a one-based page number:

```python
from mimicrag_ingestion import AdapterRegistry

def ocr_page(pdf_bytes: bytes, page: int):
    # Invoke the local or hosted OCR implementation selected by your application.
    return [{"text": "recognized text", "coordinates": (24, 80, 560, 120)}]

adapters = AdapterRegistry(ocr=ocr_page)
with open("scan.pdf", "rb") as source:
    document = adapters.parse(source.read(), "file:///scan.pdf")
```

Coordinates pass through unchanged; document the coordinate system used by the selected OCR
provider.

## CLI synchronization

```bash
python -m mimicrag_ingestion directory ./manuals \
  --server http://127.0.0.1:8080 --tenant acme --state ./directory-manifest.json
python -m mimicrag_ingestion sitemap https://example.com/sitemap.xml \
  --bearer-token "$WEB_TOKEN" --state ./web-manifest.json
python -m mimicrag_ingestion git https://github.com/acme/manuals \
  --ref main --prefix docs --state ./git-manifest.json
python -m mimicrag_ingestion s3 docs \
  --endpoint-url https://objects.example.com --prefix current/ --state ./s3-manifest.json
```

Set `--api-key` or place the key in your process wrapper. The CLI does not currently read
`MIMICRAG_API_KEY` automatically. `--bearer-token` applies to sitemap and page requests. For other
HTTP authentication schemes, construct `SitemapSource(headers=...)` in Python.

Add `--watch` to poll and incrementally synchronize at `--interval` seconds. Polling enumerates the
complete source each time, so use an interval appropriate for provider quotas.

## Programmatic connectors

Google Drive and SharePoint accept authenticated clients supplied by the host application. This
keeps OAuth credentials, refresh policy, and tenant consent outside MimicRAG:

```python
from mimicrag_ingestion import (
    GoogleDriveSource, IngestionPipeline, MimicRagSink, SharePointSource,
)

pipeline = IngestionPipeline(MimicRagSink("https://rag.example.com", api_key="..."))

# `drive_service` is an authenticated Google Drive v3 service.
pipeline.sync(
    GoogleDriveSource(drive_service).objects(),
    state_path="drive-manifest.json",
    tenant_id="acme",
)

# `graph_client.get(path)` returns a response object or decoded mapping.
pipeline.sync(
    SharePointSource(graph_client, drive_id="DRIVE_ID").objects(),
    state_path="sharepoint-manifest.json",
    tenant_id="acme",
)
```

Google-native documents are exported to text and spreadsheets to CSV. SharePoint currently lists
the selected folder's direct file children; recurse in the host application when needed. S3 uses
the standard `boto3` credential chain. Git requires the `git` executable and checks out the
requested ref in a temporary directory.

## Manifest and rename semantics

Each manifest is written atomically and contains source URI, status, document ID, content hash,
ETag/modified identity, language, warnings, errors, and the previous URI for a rename. Statuses are
`created`, `changed`, `unchanged`, `renamed`, `deleted`, `duplicate`, and `rejected`.

Keep one state file per source and tenant. Removing it discards synchronization history and makes
the next run an initial import. A rename is inferred when a new URI has the same content hash as a
source absent from the current listing; identical simultaneously present files are duplicates.

The manifest is operational state and should be backed up. It is not included in native snapshots
unless deliberately stored directly under `server.data_path`.
