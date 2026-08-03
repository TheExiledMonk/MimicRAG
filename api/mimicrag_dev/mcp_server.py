from __future__ import annotations

import json
import os
import sys
from typing import Any

from .client import Client


TOOLS = [
    {"name": "mimicrag_retrieve", "description": "Retrieve evidence passages", "inputSchema": {"type": "object", "required": ["query"], "properties": {"query": {"type": "string"}, "tenant_id": {"type": "string"}, "access_scope": {"type": "string"}, "top_k": {"type": "integer", "minimum": 1, "maximum": 100}, "filter": {"type": "object"}}}},
    {"name": "mimicrag_expand", "description": "Expand related graph context", "inputSchema": {"type": "object", "required": ["node_id"], "properties": {"node_id": {"type": "string"}, "tenant_id": {"type": "string"}, "access_scope": {"type": "string"}, "max_neighbors": {"type": "integer", "minimum": 1, "maximum": 256}}}},
    {"name": "mimicrag_ingest", "description": "Ingest approved text", "inputSchema": {"type": "object", "required": ["text", "source_uri"], "properties": {"text": {"type": "string"}, "source_uri": {"type": "string"}, "tenant_id": {"type": "string"}, "access_scope": {"type": "string"}, "title": {"type": "string"}, "format": {"enum": ["text", "markdown", "html"]}, "mode": {"enum": ["fast", "structured", "semantic"]}, "metadata": {"type": "object"}}}},
    {"name": "mimicrag_trace", "description": "Inspect a retrieval trace", "inputSchema": {"type": "object", "required": ["trace_id"], "properties": {"trace_id": {"type": "string"}}}},
]


def dispatch(client: Client, request: dict[str, Any]) -> dict[str, Any] | None:
    method, request_id = request.get("method"), request.get("id")
    if method == "notifications/initialized": return None
    if method == "initialize": result = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "mimicrag", "version": "1.5"}}
    elif method == "tools/list": result = {"tools": TOOLS}
    elif method == "tools/call":
        params = request.get("params", {}); args = params.get("arguments", {}); name = params.get("name")
        if name == "mimicrag_retrieve": value = client.retrieve(**args)
        elif name == "mimicrag_expand": value = client.expand(**args)
        elif name == "mimicrag_ingest": value = client.ingest(**args)
        elif name == "mimicrag_trace": value = client.trace(**args)
        else: raise ValueError("unknown tool: " + str(name))
        result = {"content": [{"type": "text", "text": json.dumps(value, ensure_ascii=False)}], "structuredContent": value}
    else: return {"jsonrpc": "2.0", "id": request_id, "error": {"code": -32601, "message": "method not found"}}
    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def main() -> None:
    client = Client(os.getenv("MIMICRAG_BASE_URL", "http://127.0.0.1:8080"), os.getenv("MIMICRAG_API_KEY", ""))
    for line in sys.stdin:
        try:
            response = dispatch(client, json.loads(line))
            if response is not None: print(json.dumps(response), flush=True)
        except Exception as exc:
            print(json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32000, "message": str(exc)}}), flush=True)


if __name__ == "__main__": main()
