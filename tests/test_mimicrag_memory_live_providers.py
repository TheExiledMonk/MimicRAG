import os
import unittest

from mimicrag_memory import AnthropicCompatibleMemoryModel, MiniMaxMemoryModel, MiniMaxOpenAICompatibleMemoryModel


class LiveProviderContractTests(unittest.TestCase):
    request = {"schema_version": 1, "prompt_version": "live-contract", "operation": "extract", "rules": {"evidence_ids_required": True}, "evidence": []}

    @unittest.skipUnless(os.getenv("ANTHROPIC_API_KEY"), "ANTHROPIC_API_KEY not configured")
    def test_anthropic_messages_contract(self):
        result = AnthropicCompatibleMemoryModel(os.getenv("ANTHROPIC_MODEL", "claude-haiku-4-5"), os.environ["ANTHROPIC_API_KEY"]).propose(self.request)
        self.assertEqual(result["schema_version"], 1); self.assertIsInstance(result["proposals"], list)

    @unittest.skipUnless(os.getenv("MINIMAX_API_KEY"), "MINIMAX_API_KEY not configured")
    def test_minimax_preferred_anthropic_contract(self):
        result = MiniMaxMemoryModel(os.getenv("MINIMAX_MODEL", "MiniMax-M2.7"), os.environ["MINIMAX_API_KEY"]).propose(self.request)
        self.assertEqual(result["schema_version"], 1); self.assertIsInstance(result["proposals"], list)

    @unittest.skipUnless(os.getenv("MINIMAX_OPENAI_LIVE"), "MINIMAX_OPENAI_LIVE not enabled")
    def test_minimax_openai_contract(self):
        result = MiniMaxOpenAICompatibleMemoryModel(os.getenv("MINIMAX_MODEL", "MiniMax-M2.7"), os.environ["MINIMAX_API_KEY"]).propose(self.request)
        self.assertEqual(result["schema_version"], 1); self.assertIsInstance(result["proposals"], list)


if __name__ == "__main__": unittest.main()
