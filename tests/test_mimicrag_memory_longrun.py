import tempfile
import time
import unittest
from pathlib import Path

from mimicrag_memory import MemoryNamespace, MemoryRecord, MemoryStore, Visibility
from mimicrag_memory.evaluation import evaluate_cases


class MemoryLongRunTests(unittest.TestCase):
    def test_sessions_corrections_reminders_isolation_and_metrics(self):
        with tempfile.TemporaryDirectory() as directory:
            store = MemoryStore(Path(directory) / "longrun.db")
            try:
                expected = []
                for session in range(60):
                    owner = "agent-a"; tenant = "tenant-a"
                    evidence = store.append_evidence(tenant=tenant, owner=owner, kind="task_outcome",
                        content=f"Session {session}: run check-{session} before release-{session}.", provenance=f"session:{session}")
                    memory = store.remember(MemoryRecord(tenant, owner, MemoryNamespace.PROCEDURAL,
                        f"release-{session}", f"Run check-{session} before release-{session}.", Visibility.PRIVATE,
                        "internal", ["coding", "planning"], [evidence], confidence=.9, importance=.7))
                    expected.append(memory["id"])
                correction = store.append_evidence(tenant="tenant-a", owner="agent-a", kind="correction",
                    content="Correction: release-10 now requires check-new.", provenance="session:61")
                current = store.correct(expected[10], statement="Run check-new before release-10.", correction_evidence_id=correction,
                    tenant="tenant-a", owner="agent-a")
                reminder_evidence = store.append_evidence(tenant="tenant-a", owner="agent-a", kind="conversation",
                    content="Remind me to publish when release-59 is ready.", provenance="session:62")
                reminder = store.remember(MemoryRecord("tenant-a", "agent-a", MemoryNamespace.PROSPECTIVE,
                    "release-59 ready", "Publish the release.", Visibility.PRIVATE, "internal", ["planning"],
                    [reminder_evidence], valid_from_ms=int(time.time() * 1000) - 1))
                self.assertEqual(store.prospective(tenant="tenant-a", owner="agent-a", purpose="planning", state="release-59 ready")[0]["id"], reminder["id"])
                self.assertEqual(store.recall("release-10 check", tenant="tenant-a", owner="agent-a", purpose="coding")["memories"][0]["id"], current["id"])
                self.assertFalse(store.recall("release-10", tenant="tenant-a", owner="agent-b", purpose="coding")["memories"])
                report = evaluate_cases(store, [{"query": "release-59 check-59", "tenant": "tenant-a", "owner": "agent-a", "purpose": "coding", "expected_ids": [expected[59]]}])
                self.assertEqual(report["useful_recall_rate"], 1.0)
                self.assertTrue(report["acceptance"]["zero_cross_tenant_leakage"])
            finally: store.close()


if __name__ == "__main__": unittest.main()
