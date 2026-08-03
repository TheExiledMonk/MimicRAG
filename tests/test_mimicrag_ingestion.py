import json
import tempfile
import unittest
from pathlib import Path

from mimicrag_ingestion import AdapterRegistry, DirectorySource, IngestionPipeline, ModelRoute


class FakeSink:
    def __init__(self):
        self.ingested = []
        self.deleted = []

    def ingest(self, document, tenant_id, route):
        self.ingested.append((document, tenant_id, route))
        return {"document_id": "doc-" + document.content_hash[:12]}

    def delete(self, document_id, tenant_id):
        self.deleted.append((document_id, tenant_id))
        return {"deleted": True}


class IngestionV14Tests(unittest.TestCase):
    def test_adapters_preserve_tables_email_json_and_code(self):
        adapters = AdapterRegistry()
        markdown = adapters.parse(b"# Report\n\n| Region | Total |\n|---|---|\n| EU | 4 |", "file:///report.md")
        table = next(block for block in markdown.blocks if block.type == "table")
        self.assertEqual(table.headers, ["Region", "Total"])
        self.assertEqual(table.rows, [["EU", "4"]])
        message = adapters.parse(b"From: a@example.com\nTo: b@example.com\nSubject: Hello\n\nMessage body", "file:///mail.eml")
        self.assertEqual(message.title, "Hello")
        self.assertEqual(message.metadata["email"]["from"], "a@example.com")
        data = adapters.parse(b'[{"name":"a","count":2}]', "file:///rows.json")
        self.assertEqual(data.blocks[0].headers, ["name", "count"])
        code = adapters.parse(b"def answer():\n    return 42\n", "file:///main.py")
        self.assertEqual(code.format, "source_code")

    def test_sync_detects_unchanged_rename_delete_and_deduplicates(self):
        sink = FakeSink()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); state = root / "manifest.json"; corpus = root / "corpus"; corpus.mkdir()
            (corpus / "a.md").write_text("# Hello\nThe stable content.")
            pipeline = IngestionPipeline(sink, routes={"en": ModelRoute("en", "english")})
            first = pipeline.sync(DirectorySource(corpus).objects(), state_path=state)
            self.assertEqual(first.entries[0].status, "created")
            second = pipeline.sync(DirectorySource(corpus).objects(), state_path=state)
            self.assertEqual(second.entries[0].status, "unchanged")
            (corpus / "a.md").rename(corpus / "renamed.md")
            renamed = pipeline.sync(DirectorySource(corpus).objects(), state_path=state)
            self.assertEqual(renamed.entries[0].status, "renamed")
            (corpus / "copy.md").write_text("# Hello\nThe stable content.")
            duplicate = pipeline.sync(DirectorySource(corpus).objects(), state_path=state)
            self.assertIn("duplicate", [entry.status for entry in duplicate.entries])
            (corpus / "renamed.md").unlink(); (corpus / "copy.md").unlink()
            deleted = pipeline.sync(DirectorySource(corpus).objects(), state_path=state)
            self.assertIn("deleted", [entry.status for entry in deleted.entries])
            persisted = json.loads(state.read_text())
            self.assertEqual(persisted["format"], "mimicrag-ingestion-manifest")


if __name__ == "__main__":
    unittest.main()
