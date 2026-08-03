import io
import json
import tempfile
import unittest
import urllib.request
from pathlib import Path
from unittest.mock import patch

from mimicrag_dev import Client, RetrievalSession
from mimicrag_dev.__main__ import initialize
from mimicrag_dev.mcp_server import TOOLS, dispatch

ROOT = Path(__file__).resolve().parent.parent

class FakeResponse(io.BytesIO):
    def __enter__(self): return self
    def __exit__(self, *args): self.close()

class FakeClient:
    def __init__(self): self.answers = []
    def health(self): return {"features": {"rag": True, "memory": False}}
    def answer(self, query, **options): self.answers.append((query, options)); return {"answer": "answer:" + query, "citations": []}
    def retrieve(self, **options): return {"hits": [], "request": options}
    def expand(self, **options): return {"nodes": [], "request": options}
    def ingest(self, **options): return {"document_id": "doc", "request": options}
    def trace(self, **options): return {"trace_id": options["trace_id"]}

class DeveloperExperienceV15Tests(unittest.TestCase):
    def test_openapi_paths_match_native_routes(self):
        spec = json.loads((ROOT / "docs/openapi.json").read_text()); source = (ROOT / "rag_cpp/src/http_server.cpp").read_text()
        self.assertEqual(spec["openapi"], "3.1.0"); operation_ids = []
        for path, methods in spec["paths"].items():
            literal = path.replace("{document_id}", "").replace("{trace_id}", "").replace("{job_id}", "")
            self.assertIn(literal, source, path); operation_ids.extend(value["operationId"] for value in methods.values())
        self.assertEqual(len(operation_ids), len(set(operation_ids)))

    def test_python_client_wire_shape(self):
        captured = []
        def open_request(request, timeout): captured.append((request, timeout)); return FakeResponse(b'{"hits":[]}')
        with patch.object(urllib.request, "urlopen", open_request): result = Client("https://rag.test", "secret").retrieve("policy", tenant_id="acme", top_k=3)
        request = captured[0][0]
        self.assertEqual(json.loads(request.data), {"query": "policy", "tenant_id": "acme", "top_k": 3})
        self.assertEqual(request.get_header("Authorization"), "Bearer secret"); self.assertEqual(result, {"hits": []})

    def test_sessions_are_bounded(self):
        client = FakeClient(); session = RetrievalSession(client, maximum_turns=2, maximum_context_chars=80)
        for query in ("first question", "second question", "third question"): session.ask(query)
        self.assertLessEqual(len(session.messages), 4)
        self.assertLessEqual(sum(len(item["content"]) for item in client.answers[-1][1]["conversation"]), 80)

    def test_mcp_and_portable_schemas_agree(self):
        schemas = json.loads((ROOT / "docs/function-schemas.json").read_text())
        self.assertEqual({tool["name"] for tool in TOOLS}, {item["name"] for item in schemas["functions"]})
        response = dispatch(FakeClient(), {"jsonrpc": "2.0", "id": 7, "method": "tools/call", "params": {"name": "mimicrag_retrieve", "arguments": {"query": "x"}}})
        self.assertEqual(response["id"], 7); self.assertIn("structuredContent", response["result"])
        components = dispatch(FakeClient(), {"jsonrpc": "2.0", "id": 8, "method": "tools/call", "params": {"name": "mimic_components", "arguments": {}}})
        self.assertEqual(components["result"]["structuredContent"]["features"], {"rag": True, "memory": False})

    def test_one_command_initialization(self):
        with tempfile.TemporaryDirectory() as directory:
            result = initialize(Path(directory)); config = json.loads(Path(result["config"]).read_text())
            self.assertGreaterEqual(config["calibration"]["logical_cpus"], 1); self.assertTrue(Path(result["data"]).is_dir())

    def test_compatibility_surfaces_are_present(self):
        spec = json.loads((ROOT / "docs/openapi.json").read_text()); self.assertIn("/v1/chat/completions", spec["paths"])
        for path in ("clients/cpp/mimicrag_client.h", "clients/go/mimicrag.go", "clients/rust/src/lib.rs", "clients/javascript/index.js"):
            self.assertTrue((ROOT / path).is_file(), path)

if __name__ == "__main__": unittest.main()
