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
time by about 48%.

### Disk-backed runtime indexes

The follow-up storage pass persists a 54 MiB compressed lexical snapshot, a 220 MiB
offset-addressed content store, and a 439 MiB IVF snapshot. Article/chunk text is no
longer retained in RAM: retrieval and graph expansion read only selected passages.
The IVF float32 block is memory-mapped, and duplicate sealed source-vector columns
are evicted while the active append segment remains writable. When the IVF noise guard
requests exact search, it scans all mapped vectors rather than restoring the heap copy.

Warm steady-state RSS fell from 2,308–2,425 MiB to 1,913 MiB; 477 MiB of that is
clean, reclaimable file-backed mapping, leaving 1,417 MiB private/anonymous memory.
A warm persisted-index restart became ready in 4.76 seconds.

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

After disk-backed content, mapped exact-vector fallback, and persisted lexical/IVF
loading, the same five-query loop measured 32.52 QPS sequential (26.83 ms average,
26.11 ms p50, 32.06 ms p95) and 188.21 QPS at eight-way concurrency (36.02 ms
average, 34.20 ms p50, 45.70 ms p95), with no HTTP errors.

### Interned metadata and compact graph

Document IDs, version IDs, tenant/scope, source URI, title, and JSON metadata are now
stored once per document version rather than once per chunk. Graph node types are
derived from compact ranges, chunk IDs/titles are referenced from chunk/document
records, and the node lookup table uses numeric IDs. The graph remains CSR-based and
the deep-dive endpoint returns the same wire representation.

This reduced steady RSS again, from 1,913 MiB to 1,451 MiB. Private/anonymous memory
fell from 1,417 MiB to 957 MiB; the remaining 476 MiB is clean file-backed data.
Warm startup measured 4.63 seconds. Retrieval also improved to 36.77 QPS sequential
(23.39 ms average, 22.42 ms p50, 28.50 ms p95) and 212.39 QPS at eight-way
concurrency (31.15 ms average, 29.95 ms p50, 40.54 ms p95).

### Memory-mapped BM25

The BM25 snapshot now uses a sorted fixed-width term table, contiguous strings,
fixed-width postings, and document lengths that are queried directly through `mmap`.
Newly ingested terms remain in a small heap delta and are scored together with the
mapped base. This replaces the large resident `unordered_map` while retaining normal
online ingestion and catalog-mismatch rebuilding.

The directly searchable lexical file is 184 MiB versus the former 54 MiB compressed
snapshot, but steady RSS fell to 1,248 MiB and private/anonymous memory to 570 MiB;
approximately 661 MiB is now clean, reclaimable file-backed data. Warm startup was
4.31 seconds. Replacing the per-query hash set of visible rows with a dense byte mask
then improved posting intersections substantially. Sequential retrieval measured
44.46 QPS (18.63 ms average, 18.43 ms p50, 20.39 ms p95). Three eight-way runs
measured 249.22–255.85 QPS, with 24.86–25.99 ms average latency and 31.09–36.06 ms
p95. Thus the mapped layout reduced private memory and increased throughput.

The legacy JSONL file was removed only after binary-only restart, health counters,
retrieval rankings, checksums, and concurrency tests passed. The original Wikimedia
dump and the binary catalog remain available for regeneration and further testing.
