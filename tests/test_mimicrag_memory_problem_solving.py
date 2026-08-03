import tempfile
import unittest
from pathlib import Path

from mimicrag_memory import MemoryNamespace, MemoryRecord, MemoryStore, Visibility, procedure_for_issue


class UnknownIssueFallbackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.store = MemoryStore(Path(self.tmp.name) / "fallback.db")
        self.tenant, self.owner = "tenant-a", "agent-a"

    def tearDown(self): self.store.close(); self.tmp.cleanup()

    def test_unknown_issue_gets_nonpersistent_scaffold(self):
        before = self.store.db.execute("SELECT count(*) FROM memories").fetchone()[0]
        result = procedure_for_issue(self.store, "The output becomes corrupted after the third retry",
            tenant=self.tenant, owner=self.owner, task_kind="issue")
        after = self.store.db.execute("SELECT count(*) FROM memories").fetchone()[0]
        self.assertEqual(result["status"], "unknown_issue_scaffold"); self.assertTrue(result["fallback_used"])
        self.assertFalse(result["persistent"]); self.assertFalse(result["domain_knowledge"])
        self.assertEqual(before, after); self.assertGreaterEqual(len(result["steps"]), 8)

    def test_service_and_endpoint_usage_is_excluded(self):
        for query in ("How do I call this API endpoint?", "Configure the Stripe webhook", "Use the Anthropic SDK"):
            result = procedure_for_issue(self.store, query, tenant=self.tenant, owner=self.owner, task_kind="issue")
            self.assertEqual(result["status"], "authoritative_guidance_required"); self.assertFalse(result["fallback_used"])

    def test_non_issue_task_is_excluded(self):
        result = procedure_for_issue(self.store, "Write a poem", tenant=self.tenant, owner=self.owner, task_kind="creative")
        self.assertEqual(result["status"], "not_applicable")

    def test_relevant_learned_procedure_wins(self):
        evidence = self.store.append_evidence(tenant=self.tenant, owner=self.owner, kind="task_outcome",
            content="For corrupted output, isolate the serializer and compare encoded bytes.", provenance="test")
        memory = self.store.remember(MemoryRecord(self.tenant, self.owner, MemoryNamespace.PROCEDURAL,
            "corrupted serializer output", "Isolate the serializer and compare encoded bytes.", Visibility.PRIVATE,
            "internal", ["coding"], [evidence], confidence=1, importance=1))
        result = procedure_for_issue(self.store, "corrupted serializer output", tenant=self.tenant, owner=self.owner, task_kind="issue")
        self.assertEqual(result["status"], "learned_procedure"); self.assertEqual(result["memories"][0]["id"], memory["id"])
        self.assertEqual(result["procedure_views"][0]["source"]["id"], memory["id"])


if __name__ == "__main__": unittest.main()
