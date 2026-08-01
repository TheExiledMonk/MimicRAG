from __future__ import annotations

from collections import Counter, defaultdict
from dataclasses import dataclass
import math
import re

from .models import Chunk


TOKEN_RE = re.compile(r"[\w'-]+", re.UNICODE)


def tokenize(text: str) -> list[str]:
    return [token.casefold() for token in TOKEN_RE.findall(text)]


class BM25Index:
    def __init__(self, chunks: list[Chunk], k1: float = 1.2, b: float = 0.75) -> None:
        self.chunks = {chunk.chunk_id: chunk for chunk in chunks}
        self.k1 = k1
        self.b = b
        self.lengths: dict[str, int] = {}
        self.postings: dict[str, list[tuple[str, int]]] = defaultdict(list)
        for chunk in chunks:
            terms = tokenize(chunk.text)
            self.lengths[chunk.chunk_id] = len(terms)
            for term, frequency in Counter(terms).items():
                self.postings[term].append((chunk.chunk_id, frequency))
        self.average_length = sum(self.lengths.values()) / max(1, len(self.lengths))

    def search(self, query: str, top_k: int) -> list[tuple[str, float]]:
        scores: dict[str, float] = defaultdict(float)
        total = len(self.chunks)
        for term in set(tokenize(query)):
            posting = self.postings.get(term, ())
            if not posting:
                continue
            idf = math.log(1.0 + (total - len(posting) + 0.5) / (len(posting) + 0.5))
            for chunk_id, frequency in posting:
                length = self.lengths[chunk_id]
                denominator = frequency + self.k1 * (1.0 - self.b + self.b * length / max(1.0, self.average_length))
                scores[chunk_id] += idf * frequency * (self.k1 + 1.0) / denominator
        return sorted(scores.items(), key=lambda item: (-item[1], item[0]))[:top_k]
