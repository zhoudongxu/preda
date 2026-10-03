import unittest

try:
    from .engine import run_reference
    from .adapter import native_rows_to_payload
except ImportError:  # direct invocation: python test_engine.py
    from engine import run_reference
    from adapter import native_rows_to_payload


class OccReferenceTests(unittest.TestCase):
    def test_independent_tasks_use_multiple_workers(self):
        result = run_reference(
            {"transactions": [
                {"id": "a", "source_order": 0, "scope": "a", "work": 4},
                {"id": "b", "source_order": 1, "scope": "b", "work": 4},
            ]},
            workers=2,
        )
        self.assertEqual(result["makespan"], 4)
        self.assertEqual(result["abort_count"], 0)
        self.assertTrue(result["serial_equivalent"])

    def test_conflicting_tasks_retry(self):
        result = run_reference(
            {"transactions": [
                {"id": "a", "source_order": 0, "scope": "x", "work": 3},
                {"id": "b", "source_order": 1, "scope": "x", "work": 3},
            ]},
            workers=2,
        )
        self.assertGreaterEqual(result["abort_count"], 1)
        self.assertGreaterEqual(result["retry_count"], 1)
        self.assertTrue(result["serial_equivalent"])

    def test_predecessor_cycle_is_rejected(self):
        with self.assertRaises(ValueError):
            run_reference({"transactions": [
                {"id": "a", "source_order": 0, "scope": "a", "predecessors": ["b"]},
                {"id": "b", "source_order": 1, "scope": "b", "predecessors": ["a"]},
            ]})

    def test_native_adapter_is_conservative_by_default(self):
        payload = native_rows_to_payload(
            [{
                "Target": "target-a",
                "OriginateShardIndex": 1,
                "ShardIndex": 2,
                "Function": "__relaylambda_0_f",
            }],
            workload="Token",
            variant="scheduler_fifo",
        )
        self.assertEqual(payload["source"]["effect_recovery"], "target_scope_only")
        self.assertTrue(payload["transactions"][0]["opaque"])
        self.assertEqual(payload["transactions"][0]["reads"], ["target-a"])
        self.assertEqual(payload["transactions"][0]["writes"], ["target-a"])

    def test_native_adapter_can_run_explicit_sensitivity_mode(self):
        payload = native_rows_to_payload(
            [{"Target": "a"}, {"Target": "b"}], opaque=False
        )
        result = run_reference(payload, workers=2, policy="priority")
        self.assertEqual(result["makespan"], 1)
        self.assertEqual(result["abort_count"], 0)


if __name__ == "__main__":
    unittest.main()
