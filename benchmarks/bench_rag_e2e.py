#!/usr/bin/env python3
from __future__ import annotations

import argparse
import random
import statistics
import time

from mimicrag import InMemoryRagStore, ModelConfig, RagConfig, RagRuntime, ServerConfig
from mimicrag.providers import ModelProvider


class BenchmarkProvider(ModelProvider):
    def __init__(self, dimensions=128):
        self.dimensions = dimensions
        self.config = ModelConfig("custom", "benchmark", base_url="memory://")

    def embed(self, texts, **options):
        output = []
        for text in texts:
            vector = [0.0] * self.dimensions
            for word in text.casefold().split():
                vector[hash(word) % self.dimensions] += 1.0
            output.append(vector)
        return output

    def chat(self, messages, **options):
        return "Grounded benchmark answer [1]."


def percentile(values, fraction):
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * fraction))]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--documents", type=int, default=2000)
    parser.add_argument("--queries", type=int, default=100)
    args = parser.parse_args()
    random.seed(11)
    provider = BenchmarkProvider()
    config = RagConfig(provider.config, provider.config, server=ServerConfig(requests_per_minute=0))
    runtime = RagRuntime(config, InMemoryRagStore(), provider, provider)
    topics = [f"subject_{index}" for index in range(args.documents)]
    started = time.perf_counter()
    for index, topic in enumerate(topics):
        noise = " ".join(f"word_{random.randrange(500)}" for _ in range(24))
        runtime.ingest(text=f"{topic} {topic} {topic} {noise}", source_uri=f"bench://{index}", tenant_id="bench", background=False)
    ingest_seconds = time.perf_counter() - started
    retrieval_ms, answer_ms, correct = [], [], 0
    for topic in random.sample(topics, min(args.queries, len(topics))):
        started = time.perf_counter()
        hits, _ = runtime.retrieve(topic, "bench", top_k=10)
        retrieval_ms.append((time.perf_counter() - started) * 1000)
        correct += bool(hits and topic in hits[0].chunk.text)
        started = time.perf_counter()
        runtime.answer(topic, "bench", top_k=10)
        answer_ms.append((time.perf_counter() - started) * 1000)
    runtime.close()
    print(f"documents={args.documents} queries={len(retrieval_ms)} ingest_and_index_seconds={ingest_seconds:.3f}")
    print(f"recall_at_1={correct / len(retrieval_ms):.4f}")
    print(f"retrieve_ms p50={statistics.median(retrieval_ms):.3f} p95={percentile(retrieval_ms,.95):.3f} p99={percentile(retrieval_ms,.99):.3f}")
    print(f"answer_pipeline_ms p50={statistics.median(answer_ms):.3f} p95={percentile(answer_ms,.95):.3f} p99={percentile(answer_ms,.99):.3f}")


if __name__ == "__main__":
    main()
