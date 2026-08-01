# MimicRAG English Wikipedia pilot — 2026-08-02

## Workload

- Source: `enwiki-latest-pages-articles-multistream.xml.bz2` (July 2026 dump)
- Retained: 10,000 namespace-0, non-redirect articles
- Skipped: 3,216 redirects and one empty article
- Chunks/vectors: 148,671 (768-dimensional float32 Nomic Embed Text v1.5)
- Graph edges: 930,440
- Hardware: AMD Radeon RX 7900 XTX through Vulkan; AMD Ryzen 9 7950X3D host

Ingestion completed in 919.74 seconds: 10.87 articles/s. The streaming parser read
588,785,653 decompressed source bytes and did not materialize the XML dump.

## Persistence

| Format | Bytes | Relative size | Ready after restart |
|---|---:|---:|---:|
| Decimal-vector JSONL | 2,618,772,583 | 100% | ~46 s |
| MRG1 Zstd metadata + float32 vectors | 542,590,732 | 20.72% | ~24 s |

The binary catalog is 4.83x smaller (79.28% reduction) and reduced measured restart
time by about 48%. Runtime memory remains approximately 2.3 GiB because the current
engine expands text, lexical postings, graphs, and float32 vectors into memory.

## Retrieval

The result set was checked for Java/Sun Microsystems, J. R. R. Tolkien, the Beatles,
anarchist political philosophy, and coral-reef bleaching. Expected source articles
were retained at the top; a wrong-tenant query returned zero hits. Result ordering and
scores were identical before and after binary migration.

HTTP timings include local query embedding, BM25/vector retrieval, fusion, bounded
graph expansion, JSON serialization, and loopback transport. The rate limit was raised
to 10,000 requests/minute for the concurrency run, and every response status was
validated as HTTP 200.

| Mode | Requests | QPS | Average | p50 | p95 | Errors |
|---|---:|---:|---:|---:|---:|---:|
| Sequential | 50 | 22.05 | 45.35 ms | 43.50 ms | 53.18 ms | 0 |
| 8 concurrent | 200 | 129.82 | 55.76 ms | 54.68 ms | 67.56 ms | 0 |

The legacy JSONL file was removed only after binary-only restart, health counters,
retrieval rankings, checksums, and concurrency tests passed. The original Wikimedia
dump and the binary catalog remain available for regeneration and further testing.
