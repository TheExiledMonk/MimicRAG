from __future__ import annotations

from dataclasses import dataclass
import re


@dataclass(frozen=True)
class ChunkingConfig:
    target_chars: int = 1600
    overlap_chars: int = 200
    min_chars: int = 80

    def __post_init__(self) -> None:
        if self.target_chars < 1 or self.overlap_chars < 0:
            raise ValueError("invalid chunk sizes")
        if self.overlap_chars >= self.target_chars:
            raise ValueError("overlap_chars must be smaller than target_chars")


@dataclass(frozen=True)
class TextSlice:
    text: str
    start_char: int
    end_char: int
    token_estimate: int


class TextChunker:
    def __init__(self, config: ChunkingConfig | None = None) -> None:
        self.config = config or ChunkingConfig()

    def split(self, text: str) -> list[TextSlice]:
        text = text.strip()
        if not text:
            return []
        result: list[TextSlice] = []
        start = 0
        while start < len(text):
            hard_end = min(start + self.config.target_chars, len(text))
            end = hard_end
            if hard_end < len(text):
                candidates = [text.rfind("\n\n", start, hard_end), text.rfind(". ", start, hard_end), text.rfind(" ", start, hard_end)]
                boundary = max(candidates)
                if boundary >= start + self.config.min_chars:
                    end = boundary + (2 if text[boundary:boundary + 2] in ("\n\n", ". ") else 1)
            value = text[start:end].strip()
            if value:
                actual_start = start + len(text[start:end]) - len(text[start:end].lstrip())
                result.append(TextSlice(value, actual_start, actual_start + len(value), max(1, len(re.findall(r"\w+|[^\w\s]", value)))))
            if end >= len(text):
                break
            start = max(start + 1, end - self.config.overlap_chars)
        return result
