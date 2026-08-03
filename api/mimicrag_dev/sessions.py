from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from .client import Client


@dataclass
class RetrievalSession:
    """Client-side conversation context with strict turn and character budgets."""
    client: Client
    tenant_id: str = "default"
    access_scope: str = "public"
    maximum_turns: int = 8
    maximum_context_chars: int = 12000
    messages: list[dict[str, str]] = field(default_factory=list)

    def ask(self, query: str, **options: Any) -> dict[str, Any]:
        conversation = [*self.messages, {"role": "user", "content": query}]
        while len(conversation) > self.maximum_turns * 2 or sum(len(m["content"]) for m in conversation) > self.maximum_context_chars:
            conversation.pop(0)
        result = self.client.answer(query, tenant_id=self.tenant_id, access_scope=self.access_scope,
                                    conversation=conversation, **options)
        self.messages = conversation + [{"role": "assistant", "content": result.get("answer", "")}]
        self.messages = self.messages[-self.maximum_turns * 2:]
        return result

    def clear(self) -> None: self.messages.clear()
