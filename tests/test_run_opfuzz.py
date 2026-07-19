from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("run_opfuzz", ROOT / "tools/run_opfuzz.py")
assert SPEC is not None and SPEC.loader is not None
RUN_OPFUZZ = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUN_OPFUZZ)


class OpcodeFuzzRunnerTests(unittest.TestCase):
    def test_pass_requires_positive_dispatches_and_every_failure_counter_zero(self) -> None:
        result = RUN_OPFUZZ.parse_result(
            "[opfuzz] RESULT blocks=12 seeds=4 dispatches=48 divergent_blocks=0 "
            "reports=0 fallback_skips=0 zero_charges=0 rejected_blocks=0 seed=0x1"
        )
        self.assertIsNotNone(result)
        self.assertTrue(RUN_OPFUZZ.result_passed(result))

        assert result is not None
        result["fallback_skips"] = "1"
        self.assertFalse(RUN_OPFUZZ.result_passed(result))

    def test_unrelated_output_is_not_a_result(self) -> None:
        self.assertIsNone(RUN_OPFUZZ.parse_result("[lockstep] ENABLED"))


if __name__ == "__main__":
    unittest.main()
