import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from mimicrag_memory import BuiltinWebResearcher, DreamEngine, DreamPolicy, DreamScheduler, MemoryNamespace, MemoryRecord, MemoryStore, Visibility


class FakeResearcher:
    def __init__(self): self.queries = []
    def search(self, query, *, maximum_sources, authoritative_domains):
        self.queries.append((query, maximum_sources, authoritative_domains))
        return [{"url": "https://docs.example.test/current", "title": "Current guide",
                 "summary": "Current guidance requires a verification step."}]

class FakeModel:
    provider = "test"; model = "refiner"; local = True
    def propose(self, request, **_):
        memory = request["memories"][0]
        return {"refinements": [{"memory_id": memory["id"], "operation": "add_precondition",
            "patch": {"precondition": "Confirm a clean workspace."}, "reason": "Observed prerequisite",
            "evidence_ids": memory["evidence_ids"], "confidence": .8}]}

class FakeWebResponse:
    def __enter__(self): return self
    def __exit__(self, *_): return False
    def read(self, _): return b'<a class="result__a" href="https://example.test/guide">Guide</a><div class="result__snippet">Current guidance</div>'


class DreamStateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.store = MemoryStore(Path(self.tmp.name) / "dream.db")
        self.tenant, self.owner = "tenant-a", "agent-a"
        evidence = self.store.append_evidence(tenant=self.tenant, owner=self.owner, kind="task_outcome",
            content="Build the release. Then publish it.", provenance="test")
        self.memory = self.store.remember(MemoryRecord(self.tenant, self.owner, MemoryNamespace.PROCEDURAL,
            "release", "Build the release. Then publish it.", Visibility.PRIVATE, "internal",
            ["coding"], [evidence], confidence=.9))

    def tearDown(self): self.store.close(); self.tmp.cleanup()

    def test_dream_state_is_disabled_by_default(self):
        with self.assertRaisesRegex(ValueError, "disabled"):
            DreamEngine(self.store).run(tenant=self.tenant, owner=self.owner)

    def test_dream_proposes_refinements_without_mutating_source(self):
        before = self.store.inspect(self.memory["id"], tenant=self.tenant, owner=self.owner)["memory"]
        result = DreamEngine(self.store, policy=DreamPolicy(enabled=True)).run(tenant=self.tenant, owner=self.owner, mode="deep")
        after = self.store.inspect(self.memory["id"], tenant=self.tenant, owner=self.owner)["memory"]
        self.assertEqual(before, after); self.assertEqual(result["safety"]["source_memories_modified"], 0)
        operations = {item["operation"] for item in result["refinements"]}
        self.assertIn("split_step", operations); self.assertIn("add_validation", operations)
        self.assertNotIn("replace_procedure", operations); self.assertTrue(all(item["status"] == "pending_review" for item in result["refinements"]))

    def test_approval_creates_overlay_not_replacement(self):
        result = DreamEngine(self.store, policy=DreamPolicy(enabled=True)).run(tenant=self.tenant, owner=self.owner)
        refinement = result["refinements"][0]
        reviewed = self.store.review_refinement(refinement["id"], tenant=self.tenant, owner=self.owner, decision="approved")
        view = self.store.refined_procedure(self.memory["id"], tenant=self.tenant, owner=self.owner)
        self.assertEqual(reviewed["status"], "approved"); self.assertTrue(view["immutable_source"])
        self.assertEqual(view["source"]["statement"], "Build the release. Then publish it.")
        self.assertEqual(view["approved_refinements"][0]["id"], refinement["id"])

    def test_research_is_bounded_opt_in_and_stored_as_untrusted_evidence(self):
        value = self.store.inspect(self.memory["id"], tenant=self.tenant, owner=self.owner)["memory"]
        value["recorded_at_ms"] = 1
        with self.store.db: self.store.db.execute("UPDATE memories SET record=? WHERE id=?", (json.dumps(value), self.memory["id"]))
        researcher = FakeResearcher(); policy = DreamPolicy(enabled=True, research_enabled=True,
            maximum_research_queries=1, maximum_sources_per_query=1, stale_after_days=1,
            authoritative_domains=["docs.example.test"])
        result = DreamEngine(self.store, policy=policy, researcher=researcher).run(tenant=self.tenant, owner=self.owner, mode="research")
        self.assertEqual(result["research_queries"], 1); self.assertEqual(len(researcher.queries), 1)
        annotation = next(item for item in result["refinements"] if item["operation"] == "annotate")
        evidence_id = annotation["patch"]["research_evidence_ids"][0]
        row = self.store._evidence(evidence_id, self.tenant, self.owner)
        self.assertTrue(json.loads(row["metadata"])["untrusted_external"])
        self.assertTrue(row["provenance"].startswith("dream_research:https://"))

    def test_research_requires_both_policy_and_adapter(self):
        with self.assertRaisesRegex(ValueError, "disabled"):
            DreamEngine(self.store, policy=DreamPolicy(enabled=True, research_enabled=False), researcher=FakeResearcher()).run(
                tenant=self.tenant, owner=self.owner, mode="research")

    def test_multistep_nonprocedural_memory_can_be_classified_without_retyping_source(self):
        evidence = self.store.append_evidence(tenant=self.tenant, owner=self.owner, kind="observation",
            content="Observe the failure. Then isolate the component. Finally validate the fix.", provenance="test")
        source = self.store.remember(MemoryRecord(self.tenant, self.owner, MemoryNamespace.EPISODIC,
            "debugging episode", "Observe the failure. Then isolate the component. Finally validate the fix.",
            Visibility.PRIVATE, "internal", ["coding"], [evidence]))
        result = DreamEngine(self.store, policy=DreamPolicy(enabled=True)).run(tenant=self.tenant, owner=self.owner)
        proposal = next(item for item in result["refinements"] if item["memory_id"] == source["id"] and item["operation"] == "categorize")
        self.store.review_refinement(proposal["id"], tenant=self.tenant, owner=self.owner, decision="approved")
        view = self.store.refined_procedure(source["id"], tenant=self.tenant, owner=self.owner)
        self.assertEqual(view["source"]["namespace"], "episodic"); self.assertTrue(view["immutable_source"])

    def test_safe_deterministic_category_can_be_auto_approved(self):
        result = DreamEngine(self.store, policy=DreamPolicy(enabled=True,
            auto_approve_operations={"categorize"})).run(tenant=self.tenant, owner=self.owner)
        category = next(item for item in result["refinements"] if item["operation"] == "categorize")
        self.assertEqual(category["status"], "approved"); self.assertGreater(result["auto_approved"], 0)
        self.assertEqual(self.store.inspect(self.memory["id"], tenant=self.tenant, owner=self.owner)["memory"]["statement"], "Build the release. Then publish it.")

    def test_model_assistance_only_creates_reviewable_overlay(self):
        result = DreamEngine(self.store, policy=DreamPolicy(enabled=True, model_enabled=True), model=FakeModel()).run(
            tenant=self.tenant, owner=self.owner, mode="deep")
        proposal = next(item for item in result["refinements"] if item["reason"].startswith("Model-assisted"))
        self.assertEqual(proposal["operation"], "add_precondition"); self.assertEqual(proposal["status"], "pending_review")

    def test_scheduler_runs_only_after_explicit_start(self):
        scheduler = DreamScheduler(DreamEngine(self.store, policy=DreamPolicy(enabled=True)),
            tenant=self.tenant, owner=self.owner, interval_seconds=.01)
        self.assertIsNone(scheduler.last_report)
        scheduler.interval_seconds = .01; scheduler.start()
        import time
        deadline = time.time() + 1
        while scheduler.last_report is None and time.time() < deadline: time.sleep(.01)
        scheduler.stop(); self.assertIsNotNone(scheduler.last_report)

    def test_builtin_search_is_bounded_and_zero_key(self):
        with patch("urllib.request.urlopen", return_value=FakeWebResponse()) as opened:
            sources = BuiltinWebResearcher().search("current guide", maximum_sources=1, authoritative_domains=["example.test"])
        self.assertEqual(sources[0]["url"], "https://example.test/guide"); self.assertEqual(len(sources), 1)
        self.assertIn("site%3Aexample.test", opened.call_args.args[0].full_url)


if __name__ == "__main__": unittest.main()
