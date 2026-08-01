# MimicRAG local-embedding benchmark — 2026-08-01

Release build with `MIMICDB_NATIVE_ARCH=ON`, llama.cpp `b7989`, Vulkan, and Nomic
Embed Text v1.5 Q4_K_M (768 dimensions). Host: AMD Ryzen 9 7950X3D allocation with
16 logical CPUs and AMD Radeon RX 7900 XTX. Queries use two tenants and public/private
access scopes; a filter error means an unauthorized hit escaped its predicate.

| Corpus | Ingest docs/s | Sequential QPS | p50 | p95 | p99 | 8-thread QPS | concurrent p95 | R@1 / R@10 | filter errors | replay |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 600 | 306.5 | 57.5 | 17.43 ms | 19.61 ms | 20.71 ms | 382.1 | 28.86 ms | 1.00 / 1.00 | 0 | 134.5 ms |
| 5,000 | 312.0 | 29.4 | 33.91 ms | 37.14 ms | 45.15 ms | 50.6 | 188.48 ms | 1.00 / 1.00 | 0 | 857.1 ms |

The 5,000-document run crosses the 4,096-row segment boundary and exercises custom
IVF plus its active-row overlay. It initially exposed an IVF crash: predicate-bound
construction read numeric raw storage after compression released it. The benchmark
above is the successful rerun after switching bounds to the compression-aware reader.

Reproduce with:

```bash
cmake -S . -B build-rag-bench -DCMAKE_BUILD_TYPE=Release \
  -DMIMICDB_BUILD_TESTS=OFF -DMIMICDB_BUILD_BENCHMARKS=ON \
  -DMIMICRAG_ENABLE_LLAMA=ON -DMIMICRAG_LLAMA_GPU=vulkan \
  -DMIMICDB_NATIVE_ARCH=ON
cmake --build build-rag-bench -j --target mimicrag_benchmark
build-rag-bench/rag_cpp/mimicrag_benchmark MODEL.gguf DATA_DIR 5000 300 8
```
