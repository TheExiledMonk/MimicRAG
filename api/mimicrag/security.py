from __future__ import annotations

from collections import defaultdict, deque
from dataclasses import dataclass
import re
import threading
import time


class PolicyError(ValueError):
    pass


class RateLimitError(PolicyError):
    pass


class SlidingWindowRateLimiter:
    def __init__(self, requests_per_minute: int = 120) -> None:
        self.limit = requests_per_minute
        self._events: dict[str, deque[float]] = defaultdict(deque)
        self._lock = threading.Lock()

    def check(self, identity: str, now: float | None = None) -> None:
        if self.limit <= 0:
            return
        current = time.monotonic() if now is None else now
        with self._lock:
            events = self._events[identity]
            while events and events[0] <= current - 60.0:
                events.popleft()
            if len(events) >= self.limit:
                raise RateLimitError("request rate limit exceeded")
            events.append(current)


@dataclass(frozen=True)
class InjectionAssessment:
    suspicious: bool
    patterns: tuple[str, ...]


_INJECTION_PATTERNS = {
    "instruction_override": re.compile(r"\b(ignore|disregard|forget)\b.{0,40}\b(instruction|prompt|system)\b", re.I),
    "role_impersonation": re.compile(r"\b(system|developer)\s*(message|prompt)\s*:", re.I),
    "secret_exfiltration": re.compile(r"\b(reveal|print|expose)\b.{0,40}\b(api key|secret|system prompt)\b", re.I),
}


def assess_prompt_injection(text: str) -> InjectionAssessment:
    matches = tuple(name for name, pattern in _INJECTION_PATTERNS.items() if pattern.search(text))
    return InjectionAssessment(bool(matches), matches)


def enforce_length(value: str, maximum: int, label: str) -> None:
    if len(value) > maximum:
        raise PolicyError(f"{label} exceeds {maximum} characters")
