#!/usr/bin/env python3
"""Offline acceptance tests for the Native A/B harness."""

import importlib.util
import pathlib
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("run_native_ab.py")
MODULE_SPEC = importlib.util.spec_from_file_location("run_native_ab", MODULE_PATH)
HARNESS = importlib.util.module_from_spec(MODULE_SPEC)
assert MODULE_SPEC.loader is not None
MODULE_SPEC.loader.exec_module(HARNESS)


def relay(
    target,
    *,
    origin_height=1,
    destination_height=2,
    origin_shard=0,
    destination_shard=1,
    function="__relaylambda_1_source",
):
    return {
        "Arguments": {"value": target},
        "BuildNum": 1,
        "Contract": "Example",
        "Function": function,
        "Height": destination_height,
        "Initiator": "initiator",
        "InvokeContextType": "RelayInbound",
        "InvokeResult": "Success",
        "OriginateHeight": origin_height,
        "OriginateShardIndex": origin_shard,
        "OriginateShardOrder": 2,
        "ShardIndex": destination_shard,
        "ShardOrder": 2,
        "Target": target,
    }


def block(shard, height, transactions):
    return {
        "ShardIndex": shard,
        "Height": height,
        "TxnCount": len(transactions),
        "ConfirmTxn": transactions,
    }


def empty_feature_counters():
    return {name: 0 for name in HARNESS.FEATURE_COUNTERS}


class DependencyProjectionTests(unittest.TestCase):
    def projection(self, blocks, workload="Token"):
        return HARNESS.dependency_shape_projection(blocks, workload)

    def test_reorder_inside_one_destination_batch_is_detected(self):
        first = relay("target-a")
        second = relay("target-b")
        ordered = self.projection([block(1, 2, [first, second])])
        reordered = self.projection([block(1, 2, [second, first])])
        self.assertNotEqual(ordered, reordered)

    def test_destination_block_relocation_is_ignored(self):
        original = relay("target-a", destination_height=2)
        relocated = relay("target-a", destination_height=9)
        self.assertEqual(
            self.projection([block(1, 2, [original])]),
            self.projection([block(1, 9, [relocated])]),
        )

    def test_independent_producer_interleaving_is_ignored(self):
        producer_a = relay("target-a", origin_height=1)
        producer_b = relay("target-b", origin_height=7)
        self.assertEqual(
            self.projection([block(1, 8, [producer_a, producer_b])]),
            self.projection([block(1, 8, [producer_b, producer_a])]),
        )

    def test_origin_block_membership_change_is_detected(self):
        first = relay("target-a", origin_height=1)
        same_origin = relay("target-b", origin_height=1)
        other_origin = relay("target-b", origin_height=2)
        self.assertNotEqual(
            self.projection([block(1, 3, [first, same_origin])]),
            self.projection([block(1, 3, [first, other_origin])]),
        )

    def test_kitty_register_aggregation_order_is_explicitly_unordered(self):
        first = relay(
            "target-a", function="__relaylambda_7_registerNewBorns"
        )
        second = relay(
            "target-b", function="__relaylambda_7_registerNewBorns"
        )
        self.assertEqual(
            self.projection([block(1, 2, [first, second])], "Kitty"),
            self.projection([block(1, 2, [second, first])], "Kitty"),
        )


class MeasurementGateTests(unittest.TestCase):
    def workload(self):
        return {
            "expected_runtime_features": {
                "relay_activity": True,
                "generic_batch": True,
                "verified_reserve": True,
            }
        }

    def active_lifetime_counters(self):
        counters = empty_feature_counters()
        counters.update(
            {
                "plan_loads": 1,
                "optimization_eligible_invocations": 1,
                "relay_buffer_reserve_calls": 1,
                "queue_batch_push_calls": 1,
                "queue_batch_elements": 1,
                "logical_relay_emissions": 1,
            }
        )
        return counters

    def report(self, window_counters):
        return {
            "report_schema_version": 2,
            "counters": self.active_lifetime_counters(),
            "measurement_window": {
                "status": "completed",
                "metrics_available": True,
                "counters": window_counters,
                "timings_ns": {},
                "derived": {},
            },
        }

    def test_schema_v1_is_rejected_for_performance_only(self):
        legacy = {
            "report_schema_version": 1,
            "counters": empty_feature_counters(),
        }
        self.assertTrue(HARNESS.measurement_window_validation(legacy)[0])
        self.assertFalse(
            HARNESS.measurement_window_validation(
                legacy, require_completed_schema_v2=True
            )[0]
        )
        self.assertEqual(
            HARNESS.selected_measurement_scope(legacy),
            ({}, {}, {}, "unavailable"),
        )

    def test_setup_activity_cannot_satisfy_performance_gate(self):
        report = self.report(empty_feature_counters())
        self.assertEqual(
            HARNESS.validate_variant_activity(
                self.workload(),
                "verified_reserve_plus_batch",
                report,
                require_measurement_window=False,
            ),
            [],
        )
        issues = HARNESS.validate_variant_activity(
            self.workload(),
            "verified_reserve_plus_batch",
            report,
            require_measurement_window=True,
        )
        self.assertTrue(issues)
        self.assertTrue(
            any("measurement_window relay_buffer_reserve_calls" in issue
                for issue in issues)
        )

    def test_window_activity_passes_with_lifetime_plan_load(self):
        window = self.active_lifetime_counters()
        window["plan_loads"] = 0
        report = self.report(window)
        self.assertTrue(
            HARNESS.measurement_window_validation(
                report, require_completed_schema_v2=True
            )[0]
        )
        self.assertEqual(
            HARNESS.validate_variant_activity(
                self.workload(),
                "verified_reserve_plus_batch",
                report,
                require_measurement_window=True,
            ),
            [],
        )


if __name__ == "__main__":
    unittest.main()
