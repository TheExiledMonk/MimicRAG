import tempfile
import unittest
from pathlib import Path

from mimicrag_memory import DreamEngine, DreamPolicy, MemoryNamespace, MemoryRecord, MemoryStore, Visibility


class GeneralMemoryLifecycleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.store = MemoryStore(Path(self.tmp.name) / "general.db")
        self.tenant, self.owner = "tenant-a", "agent-a"

    def tearDown(self): self.store.close(); self.tmp.cleanup()

    def remember(self, namespace, subject, statement, *, sensitivity="internal", confirmed=False, kind="conversation"):
        evidence = self.store.append_evidence(tenant=self.tenant, owner=self.owner, kind=kind,
            content=statement, provenance="explicit-test", sensitivity=sensitivity)
        memory = self.store.remember(MemoryRecord(self.tenant, self.owner, namespace, subject, statement,
            Visibility.PRIVATE, sensitivity, ["conversation", "planning", "research", "coding"], [evidence]), confirmed=confirmed)
        return memory, evidence

    def test_profile_project_research_and_procedure_share_full_lifecycle(self):
        profile, _ = self.remember(MemoryNamespace.SEMANTIC, "user name and country",
            "The user's name is Fabian and the user lives in the Philippines.", sensitivity="personal")
        self.assertEqual(profile["status"], "pending_confirmation")
        self.store.confirm(profile["id"], tenant=self.tenant, owner=self.owner)
        private_recall = self.store.recall("Fabian Philippines", tenant=self.tenant, owner=self.owner,
            purpose="conversation", sensitivity_allowed=("public", "internal", "personal"))
        self.assertEqual(private_recall["memories"][0]["id"], profile["id"])

        project, _ = self.remember(MemoryNamespace.SEMANTIC, "book project",
            "Fabian is writing a historical science-fiction book about navigation.")
        research, _ = self.remember(MemoryNamespace.SEMANTIC, "book research",
            "Research notes compare Polynesian navigation with early astronomical instruments.", kind="observation")
        procedure, _ = self.remember(MemoryNamespace.PROCEDURAL, "research workflow",
            "Collect primary sources. Then compare claims. Finally record citations.", kind="task_outcome")
        for query, identifier, purpose in (("historical science-fiction book", project["id"], "planning"),
                                            ("Polynesian astronomical navigation", research["id"], "research"),
                                            ("collect compare citations", procedure["id"], "research")):
            recalled = self.store.recall(query, tenant=self.tenant, owner=self.owner, purpose=purpose)
            self.assertEqual(recalled["memories"][0]["id"], identifier)

        correction = self.store.append_evidence(tenant=self.tenant, owner=self.owner, kind="correction",
            content="The book now focuses on maritime navigation in speculative history.", provenance="explicit-correction")
        current = self.store.correct(project["id"], statement="The book focuses on maritime navigation in speculative history.",
            correction_evidence_id=correction, tenant=self.tenant, owner=self.owner)
        self.assertEqual(self.store.recall("maritime speculative history", tenant=self.tenant, owner=self.owner,
            purpose="planning")["memories"][0]["id"], current["id"])

        dream = DreamEngine(self.store, policy=DreamPolicy(enabled=True)).run(tenant=self.tenant, owner=self.owner, mode="deep")
        categories = {item["memory_id"]: item["patch"]["category"] for item in dream["refinements"] if item["operation"] == "categorize"}
        self.assertEqual(categories[profile["id"]], "profile"); self.assertEqual(categories[research["id"]], "research")
        self.assertEqual(categories[procedure["id"]], "procedure")

        self.store.forget(research["id"], tenant=self.tenant, owner=self.owner, erase_evidence=True)
        self.assertNotIn(research["id"], {item["id"] for item in self.store.recall("Polynesian navigation",
            tenant=self.tenant, owner=self.owner, purpose="research")["memories"]})


if __name__ == "__main__": unittest.main()
