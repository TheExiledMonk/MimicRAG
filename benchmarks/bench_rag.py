#!/usr/bin/env python3
"""Deterministic end-to-end retrieval benchmark with recall measurement."""

from __future__ import annotations

import argparse
import random
import statistics
import time

from mimicrag import EmbeddingIndexer, HybridRetriever, InMemoryRagStore, Ingestor, RetrievalConfig
from mimicrag.config import ModelConfig
from mimicrag.providers import ModelProvider


class HashEmbedding(ModelProvider):
    def __init__(self, dimensions: int = 128):
        self.dimensions = dimensions
        self.config = ModelConfig("custom", f"hash-{dimensions}", base_url="benchmark://")

    def chat(self, messages, **options): return ""

    def embed(self, texts, **options):
        vectors = []
        for text in texts:
            vector = [0.0] * self.dimensions
            for word in text.casefold().split():
                vector[hash(word) % self.dimensions] += 1.0
            vectors.append(vector)
        return vectors


def percentile(values, fraction):
    return sorted(values)[min(len(values) - 1, int(len(values) * fraction))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--documents", type=int, default=2000)
    parser.add_argument("--queries", type=int, default=100)
    args = parser.parse_args()
    random.seed(7)
    store = InMemoryRagStore()
    ingestor = Ingestor(store)
    topics = [f"topic_{index}" for index in range(args.documents)]
    for index, topic in enumerate(topics):
        noise = " ".join(f"word_{random.randrange(1000)}" for _ in range(30))
        ingestor.ingest(text=f"{topic} {topic} {topic} {noise}", source_uri=f"bench://{index}", tenant_id="bench")
    provider = HashEmbedding()
    started = time.perf_counter()
    indexed = EmbeddingIndexer(store, provider, batch_size=256).index_tenant("bench")
    index_seconds = time.perf_counter() - started
    retriever = HybridRetriever(store, provider, RetrievalConfig(top_k=10))
    latencies = []
    correct = 0
    for topic in random.sample(topics, min(args.queries, len(topics))):
        started = time.perf_counter()
        hits = retriever.search(topic, "bench", 10)
        latencies.append((time.perf_counter() - started) * 1000)
        correct += bool(hits and topic in hits[0].chunk.text)
    print(f"documents={args.documents} queries={len(latencies)} embedded={indexed.embedded}")
    print(f"index_seconds={index_seconds:.3f} recall_at_1={correct / len(latencies):.4f}")
    print(f"latency_ms p50={statistics.median(latencies):.3f} p95={percentile(latencies, .95):.3f} p99={percentile(latencies, .99):.3f}")


if __name__ == "__main__":
    main()
