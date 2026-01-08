from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class WritePolicy:
    quorum: int | None = None


@dataclass
class ReadPolicy:
    consistency: str = "any"
    verify: bool = False


@dataclass
class FailurePolicy:
    max_retries: int = 2
    blacklist_ttl_sec: float = 5.0


@dataclass
class ApiPolicy:
    write_policy: WritePolicy = field(default_factory=WritePolicy)
    read_policy: ReadPolicy = field(default_factory=ReadPolicy)
    failure_policy: FailurePolicy = field(default_factory=FailurePolicy)
