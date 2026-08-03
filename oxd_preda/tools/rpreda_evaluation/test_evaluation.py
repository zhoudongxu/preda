#!/usr/bin/env python3

import copy
import json
import pathlib
import tempfile
import unittest

from CoverageAnalyzer import (
    ManifestValidationError,
    UNKNOWN_REASON_CATEGORIES,
    analyze_manifest,
    classify_unknown_reasons,
    validate_manifest,
)
from SyntheticPredaGenerator import GeneratorConfigurationError, generate_program
from CoverageScalabilityRunner import (
    ANALYSIS_PHASES,
    EvaluationError,
    REQUIRED_SCALABILITY_BUILD_FEATURES,
    _build_round_schedule,
    _case_configurations,
    _distribution,
    _sample_metrics,
    _timing_stability_control,
)


def formula(kind="BoolLiteral", sort="Bool"):
    return {
        "kind": kind, "sort": {"kind": sort}, "children": [],
        "operator": "", "source_text": "", "literal_value": "true",
    }


def bound(identifier, status="Conservative", finite=True, reason="finite upper bound"):
    return {
        "certificate_id": identifier,
        "status": status,
        "exact": {"kind": "unknown", "reason": "conditional occurrence"},
        "upper_bound": {"kind": "constant", "value": 1} if finite else {"kind": "unknown", "reason": "loop occurrence unsupported"},
        "reason": reason,
        "supporting_cfg_fact_ids": ["root"],
        "supporting_constraint_ids": [],
        "supporting_solver_result_ids": [],
    }


def minimal_manifest():
    direct = bound("work.direct")
    transitive = bound("work.transitive")
    depth = bound("depth")
    physical = {
        "certificate_id": "work.physical",
        "status": "Conservative",
        "bound_kind": "Constant",
        "constant_term": 1,
        "active_shard_count_coefficient": 0,
        "expression": "1",
        "reason": "finite route bound",
        "supporting_cfg_fact_ids": ["root"],
        "supporting_constraint_ids": [],
        "supporting_solver_result_ids": [],
    }
    leaf_physical = copy.deepcopy(physical)
    leaf_physical["certificate_id"] = "leaf.physical"
    leaf_physical["supporting_cfg_fact_ids"] = ["leaf"]
    return {
        "schema_version": 5,
        "dapp": "test",
        "contract": "C",
        "artifact_binding": {
            "binding_complete": True,
            "dapp": "test",
            "contract": "C",
            "transpiler_version": "test",
            "intermediate_hash": "intermediate",
            "module_id": "module",
            "module_hash_kind": "preda_module_id",
            "module_hash": "module",
            "manifest_hash_algorithm": "sha256",
            "manifest_hash": "hash",
        },
        "relay_sites": [{
            "id": "site", "source_function_id": "root", "source_function": "root", "source_function_signature": "root(uint32)",
            "relay_kind": "custom_scope", "target_scope": "uint32", "handler_id": "handler", "arguments": [], "branches": [], "loops": [],
        }],
        "handlers": [{"id": "handler", "resolved": True, "target_function_id": "leaf", "kind": "named"}],
        "edges": [{"id": "edge", "source_function_id": "root", "relay_site_id": "site", "handler_id": "handler", "resolved": True}],
        "functions": [
            {"source_function_id": "root", "relay_site_ids": ["site"], "summary": {}},
            {"source_function_id": "leaf", "relay_site_ids": [], "summary": {}},
        ],
        "refinement": {
            "symbols": [],
            "constraints": [
                {"id": "target.constraint", "kind": "RelayTargetRelation", "source_function_id": "root", "formula": formula()},
                {"id": "count.constraint", "kind": "RelayCountEquality", "source_function_id": "root", "formula": formula()},
                {"id": "nonnegative.root", "kind": "RelayCountNonNegative", "source_function_id": "root", "formula": formula()},
                {"id": "nonnegative.leaf", "kind": "RelayCountNonNegative", "source_function_id": "leaf", "formula": formula()},
            ],
            "proof_obligations": [
                {"id": "target.obligation", "kind": "RelayTargetEquality", "status": "Generated", "source_function_id": "root", "relay_site_id": "site", "goal": formula(), "constraint_ids": ["target.constraint"]},
                {"id": "count.root", "kind": "RelayCountEquality", "status": "Generated", "source_function_id": "root", "goal": formula(), "constraint_ids": ["count.constraint"]},
                {"id": "count.leaf", "kind": "RelayCountEquality", "status": "Generated", "source_function_id": "leaf", "goal": formula(), "constraint_ids": []},
            ],
        },
        "control_flow": {
            "extension_schema_version": 1,
            "functions": [
                {"function_id": "root", "generated_relay_lambda": False, "status": "Complete", "nodes": [{"id": "root", "kind": "RelayEmit", "relay_site_id": "site"}]},
                {"function_id": "leaf", "generated_relay_lambda": False, "status": "Complete", "nodes": [{"id": "leaf", "kind": "Basic"}]},
            ],
            "synchronous_call_graph": {"edges": [], "analysis": {"relay_reachable": {"root": True, "leaf": False}}},
            "relay_icfg": {
                "function_analyses": [{"function_id": "root", "status": "Complete"}, {"function_id": "leaf", "status": "Complete"}],
                "synchronous_composition_analyses": [],
                "interprocedural_edges": [{"id": "async", "kind": "AsyncRelaySpawn", "source_function_id": "root", "resolved": True}],
            },
        },
        "parallel_certificate": {
            "extension_schema_version": 1,
            "functions": [
                {
                    "source_function_id": "root", "certificate_status": "Conservative", "pair_relations": [],
                    "direct_logical_work": direct, "transitive_logical_work": transitive,
                    "physical_route_work": physical, "relay_tree_depth": depth,
                },
                {
                    "source_function_id": "leaf", "certificate_status": "Complete", "pair_relations": [],
                    "direct_logical_work": bound("leaf.direct", "Complete"),
                    "transitive_logical_work": bound("leaf.transitive", "Complete"),
                    "physical_route_work": leaf_physical, "relay_tree_depth": bound("leaf.depth", "Complete"),
                },
            ],
        },
    }


class EvaluationTests(unittest.TestCase):
    def test_round_major_schedule_is_deterministic_and_complete(self):
        case_ids = ["a", "b", "c", "d"]
        first = _build_round_schedule(case_ids, 1, 3, 88)
        second = _build_round_schedule(case_ids, 1, 3, 88)
        self.assertEqual(first, second)
        self.assertEqual([value["round_kind"] for value in first], ["warmup", "measured", "measured", "measured"])
        for scheduled_round in first:
            self.assertEqual(set(scheduled_round["case_order"]), set(case_ids))
            self.assertEqual(len(scheduled_round["case_order"]), len(case_ids))

    def test_width_sweep_uses_one_function_floor(self):
        config = json.loads(
            (pathlib.Path(__file__).parent / "benchmarks.json").read_text(
                encoding="utf-8"
            )
        )
        width_cases = [
            case for _, axis, case in _case_configurations(config)
            if axis == "relay_width"
        ]
        self.assertEqual({case["functions"] for case in width_cases}, {17})
        self.assertEqual({generate_program(case)[1]["derived"]["functions"] for case in width_cases}, {17})

    def test_distributions_disclose_small_sample_tail_and_stability(self):
        distribution = _distribution([1, 2, 3, 4, 5])
        self.assertEqual(distribution["median"], 3.0)
        self.assertEqual(distribution["iqr"], 2.0)
        self.assertTrue(distribution["p95_is_descriptive"])
        self.assertFalse(distribution["tail_inference_supported"])
        samples = [
            {
                "baseline_configuration": True,
                "normalized_manifest_sha256": "same",
                "case_id": case_id,
                "round_index": round_index,
                "total_analysis_ms": elapsed,
            }
            for round_index, values in enumerate(((10.0, 11.0), (12.0, 13.0)))
            for case_id, elapsed in zip(("a", "b"), values)
        ]
        control = _timing_stability_control(samples)
        self.assertTrue(control["available"])
        self.assertAlmostEqual(control["round_median_max_over_min"], 12.5 / 10.5)
        self.assertIn(control["status"], ("WithinAdvisoryThreshold", "ExceedsAdvisoryThreshold"))

    def test_scalability_rejects_trace_enabled_driver(self):
        manifest = minimal_manifest()
        manifest.pop("artifact_binding")
        profile = {
            "schema_version": 1,
            "status": "Compiled",
            "profiling_enabled": True,
            "phase_times_ms": {name: 0.0 for name in ANALYSIS_PHASES},
            "total_analysis_ms": 0.0,
            "build_features": dict(REQUIRED_SCALABILITY_BUILD_FEATURES),
        }
        profile["build_features"]["runtime_trace"] = True
        process = {
            "schema_version": 1,
            "returncode": 0,
            "timed_out": False,
            "launch_error": "",
            "peak_rss_kib": 1,
            "wall_time_ms": 0.0,
        }
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw)
            paths = [root / name for name in ("manifest.json", "metrics.json", "process.json")]
            for path, value in zip(paths, (manifest, profile, process)):
                path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaisesRegex(EvaluationError, "build-feature mismatch"):
                _sample_metrics(*paths)

    def covered_trace(self, occurrences=1, mismatch=False):
        validations = [{
            "status": "Passed", "check_kind": "artifact_binding", "reason": "bound",
            "detail": {
                "root_trace_tx_id": 0, "current_trace_tx_id": 0,
                "module_identity": "module",
                "manifest_identity": {"binding_complete": True, "module_id": "module", "manifest_hash": "hash"},
            },
        }]
        certificate_ids = {
            "certificate_direct_work": "work.direct",
            "certificate_transitive_work": "work.transitive",
            "certificate_physical_work": "work.physical",
            "certificate_depth": "depth",
        }
        for kind, certificate_id in certificate_ids.items():
            validations.append({
                "status": "Mismatch" if mismatch and kind == "certificate_depth" else "Passed",
                "check_kind": kind,
                "reason": "runtime bound check",
                "detail": {
                    "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                    "module_identity": "module", "function": "root",
                    "certificate_id": certificate_id, "property_id": certificate_id,
                },
            })
        emissions = []
        routes = []
        executions = [{
            "trace_tx_id": 1, "root_trace_tx_id": 1, "parent_trace_tx_id": 0,
            "source_function_id": "root", "module_id": "module", "is_relay": False,
            "started": True, "completed": True, "succeeded": True,
        }]
        for occurrence in range(occurrences):
            child_id = occurrence + 2
            emissions.append({
                "child_trace_tx_id": child_id,
                "root_trace_tx_id": 1, "parent_trace_tx_id": 1, "source_module_id": "module",
                "source_function_id": "root", "relay_site_id": "site", "relay_site_ordinal": 0,
                "occurrence_index": occurrence, "emission_sequence": occurrence,
            })
            routes.append({
                "physical_trace_tx_id": child_id,
                "root_trace_tx_id": 1, "parent_trace_tx_id": 1,
                "source_module_id": "module", "relay_site_id": "site", "relay_site_ordinal": 0,
                "occurrence_index": occurrence, "target_shard": occurrence,
            })
            executions.append({
                "trace_tx_id": child_id, "root_trace_tx_id": 1, "parent_trace_tx_id": 1,
                "source_function_id": "leaf", "module_id": "module", "is_relay": True,
                "started": True, "completed": True, "succeeded": True,
            })
            validations.append({
                "status": "Passed", "check_kind": "relay_site_identity", "reason": "identity",
                "detail": {
                    "root_trace_tx_id": 1, "current_trace_tx_id": 1, "module_identity": "module",
                    "function": "root",
                    "relay_site_id": "site", "relay_site_ordinal": 0, "occurrence_index": occurrence,
                },
            })
        return {
            "report_schema_version": 1,
            "counters": {
                "logical_relay_emissions": occurrences,
                "physical_relay_routes": occurrences,
                "source_transactions_observed": 1,
                "relay_executions": occurrences,
                "microtransactions_observed": occurrences + 1,
                "instrumentation_failures": 0,
            },
            "logical_relay_emissions": emissions,
            "physical_relay_routes": routes,
            "relay_executions": executions,
            "validation_results": validations,
        }

    def test_conservative_finite_bound_is_proved_and_site_is_linked(self):
        result = analyze_manifest("C", minimal_manifest())
        self.assertEqual(result["protocol_coverage"]["relay_sites_covered"]["covered"], 1)
        self.assertEqual(result["certificate_quality"]["counts"]["WorkBound"]["Proved"], 3)
        self.assertEqual(result["certificate_quality"]["counts"]["DepthBound"]["Proved"], 1)

    def test_depth_formula_gap_is_explicit(self):
        value = analyze_manifest("C", minimal_manifest())["refinement_coverage"]["depth_constraints"]
        self.assertFalse(value["available_in_refinement_ir"])
        self.assertEqual(value["covered"], 0)
        self.assertIn("relay_tree_depth", value["represented_elsewhere"])

    def test_output_schema_keeps_bounds_taxonomy_and_unavailable_runtime_typed(self):
        result = analyze_manifest("C", minimal_manifest())
        for record in result["certificate_quality"]["records"]:
            if record["property"] in ("WorkBound", "DepthBound"):
                self.assertIsInstance(record["upper_bound"], dict)
        self.assertEqual(
            set(result["certificate_quality"]["unknown_reason_breakdown"]),
            set(UNKNOWN_REASON_CATEGORIES),
        )
        runtime = result["runtime_weighted_coverage"]
        self.assertFalse(runtime["available"])
        self.assertIn("observed_distinct_site", runtime["relay_pairs"])
        self.assertIn("all_pair_instance_percentage", runtime["relay_pairs"])
        self.assertIn("details", runtime["relay_trees"])

    def test_analysis_only_manifest_never_requires_fabricated_binding(self):
        manifest = minimal_manifest()
        manifest.pop("artifact_binding")
        with self.assertRaises(ManifestValidationError):
            validate_manifest(manifest)
        validate_manifest(manifest, require_artifact_binding=False)

    def test_unknown_reason_prefers_structural_cause(self):
        reasons = classify_unknown_reasons(["no finite bound", "loop occurrence unsupported"])
        self.assertEqual(reasons[0], "loop")

    def test_generator_is_deterministic_and_exact(self):
        config = {
            "functions": 8, "statements": 24, "relay_sites": 4, "relay_width": 2,
            "relay_depth": 2, "branch_depth": 1, "loop_depth": 1,
            "sync_call_depth": 2, "arguments_per_relay": 2,
        }
        first, first_meta = generate_program(config)
        second, second_meta = generate_program(config)
        self.assertEqual(first, second)
        self.assertEqual(first_meta, second_meta)
        self.assertEqual(first_meta["derived"]["relay_sites"], 4)
        self.assertEqual(first_meta["derived"]["statements"], 24)

    def test_generator_rejects_infeasible_relay_graph(self):
        with self.assertRaises(GeneratorConfigurationError):
            generate_program({"functions": 8, "statements": 30, "relay_sites": 2, "relay_width": 1, "relay_depth": 3})

    def test_zero_runtime_pair_denominator_is_null(self):
        manifest = minimal_manifest()
        trace = {
            "report_schema_version": 1,
            "counters": {
                "logical_relay_emissions": 0, "physical_relay_routes": 0,
                "source_transactions_observed": 0, "relay_executions": 0,
                "microtransactions_observed": 0, "instrumentation_failures": 0,
            },
            "logical_relay_emissions": [],
            "physical_relay_routes": [],
            "relay_executions": [],
            "validation_results": [{"status": "Passed", "check_kind": "artifact_binding", "detail": {}}],
        }
        value = analyze_manifest("C", manifest, trace)["runtime_weighted_coverage"]["relay_pairs"]
        self.assertIsNone(value["percentage"])

    def test_runtime_complete_uses_four_bounds_and_emission_identity(self):
        value = analyze_manifest("C", minimal_manifest(), self.covered_trace())["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 1)
        self.assertEqual(value["relay_trees"]["finite_work_bound"], 1)
        self.assertEqual(value["transactions"]["complete"], 1)
        # Physical routing/execution records do not inflate logical emissions.
        self.assertEqual(value["relay_emissions"]["observed"], 1)

    def test_same_site_occurrences_are_retained_but_not_certified(self):
        value = analyze_manifest("C", minimal_manifest(), self.covered_trace(3))["runtime_weighted_coverage"]
        self.assertEqual(value["relay_pairs"]["observed"], 3)
        self.assertEqual(value["relay_pairs"]["observed_distinct_site"], 0)
        self.assertIsNone(value["relay_pairs"]["percentage"])
        self.assertEqual(value["transactions"]["partial"], 1)

    def test_runtime_mismatch_forces_fallback(self):
        value = analyze_manifest("C", minimal_manifest(), self.covered_trace(mismatch=True))["runtime_weighted_coverage"]
        self.assertEqual(value["transactions"]["fallback"], 1)

    def test_trace_counter_mismatch_makes_runtime_coverage_unavailable(self):
        trace = self.covered_trace()
        trace["counters"]["logical_relay_emissions"] = 0
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertFalse(value["available"])
        self.assertIn("logical_relay_emissions", value["reason"])

    def test_invalid_instrumentation_counter_is_rejected_without_crashing(self):
        trace = self.covered_trace()
        trace["counters"]["instrumentation_failures"] = "not-an-integer"
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertFalse(value["available"])
        self.assertIn("instrumentation_failures", value["reason"])

    def test_runtime_slice_keeps_sync_helper_emission_and_pair(self):
        manifest = minimal_manifest()
        helper_site = copy.deepcopy(manifest["relay_sites"][0])
        helper_site.update({
            "id": "helper.site", "source_function_id": "helper",
            "source_function": "helper", "source_function_signature": "helper()",
            "handler_id": "helper.handler",
        })
        manifest["relay_sites"].append(helper_site)
        manifest["handlers"].append({
            "id": "helper.handler", "resolved": True,
            "target_function_id": "leaf", "kind": "named",
        })
        manifest["edges"].append({
            "id": "helper.edge", "source_function_id": "helper",
            "relay_site_id": "helper.site", "handler_id": "helper.handler",
            "resolved": True,
        })
        manifest["functions"].append({
            "source_function_id": "helper", "relay_site_ids": ["helper.site"], "summary": {},
        })
        manifest["control_flow"]["functions"].append({
            "function_id": "helper", "generated_relay_lambda": False,
            "status": "Complete",
            "nodes": [{"id": "helper.node", "kind": "RelayEmit", "relay_site_id": "helper.site"}],
        })
        manifest["control_flow"]["synchronous_call_graph"]["edges"].append({
            "id": "sync.edge", "kind": "Synchronous", "caller": "root",
            "callee": "helper", "resolved": True,
        })
        manifest["control_flow"]["relay_icfg"]["function_analyses"].append({
            "function_id": "helper", "status": "Complete",
        })
        root_certificate = manifest["parallel_certificate"]["functions"][0]
        root_certificate["direct_logical_work"]["upper_bound"]["value"] = 2
        manifest["refinement"]["proof_obligations"].extend([
            {
                "id": "pair.root.helper.mutual", "kind": "RelayMutualExclusion",
                "source_function_id": "root", "relay_site_id": "site",
                "related_relay_site_id": "helper.site", "constraint_ids": [],
                "goal": formula(), "status": "Generated",
                "solver_result": {"status": "Disproved"},
            },
            {
                "id": "pair.root.helper.target", "kind": "RelayTargetIndependence",
                "source_function_id": "root", "relay_site_id": "site",
                "related_relay_site_id": "helper.site", "constraint_ids": [],
                "goal": formula(), "status": "Generated",
                "solver_result": {"status": "Proved"},
            },
        ])
        root_certificate["pair_relations"] = [
            {
                "certificate_id": "pair.root.helper", "status": "Proved",
                "source_function_id": "root",
                "relation": "MustPrecedeAB", "site_a": "site", "site_b": "helper.site",
                "supporting_cfg_fact_ids": ["root"],
                "supporting_constraint_ids": [], "supporting_solver_result_ids": [],
            },
            {
                "certificate_id": "pair.root.helper.independent", "status": "Proved",
                "source_function_id": "root",
                "relation": "CoEmissionIndependent", "site_a": "site", "site_b": "helper.site",
                "supporting_cfg_fact_ids": ["root"],
                "supporting_constraint_ids": [],
                "supporting_solver_result_ids": [
                    "pair.root.helper.mutual", "pair.root.helper.target",
                ],
            },
        ]

        trace = self.covered_trace()
        trace["logical_relay_emissions"].append({
            "child_trace_tx_id": 3, "root_trace_tx_id": 1, "parent_trace_tx_id": 1,
            "source_module_id": "module", "source_function_id": "helper",
            "relay_site_id": "helper.site", "relay_site_ordinal": 1,
            "occurrence_index": 0, "emission_sequence": 1,
        })
        trace["physical_relay_routes"].append({
            "physical_trace_tx_id": 3, "root_trace_tx_id": 1, "parent_trace_tx_id": 1,
            "source_module_id": "module", "relay_site_id": "helper.site",
            "relay_site_ordinal": 1, "occurrence_index": 0, "target_shard": 0,
        })
        trace["relay_executions"].append({
            "trace_tx_id": 3, "root_trace_tx_id": 1, "parent_trace_tx_id": 1,
            "source_function_id": "leaf", "module_id": "module", "is_relay": True,
            "started": True, "completed": True, "succeeded": True,
        })
        trace["counters"].update({
            "logical_relay_emissions": 2,
            "physical_relay_routes": 2,
            "relay_executions": 2,
            "microtransactions_observed": 3,
        })
        trace["validation_results"].extend([
            {
                "status": "Passed", "check_kind": "relay_site_identity", "reason": "identity",
                "detail": {
                    "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                    "module_identity": "module", "function": "helper",
                    "relay_site_id": "helper.site", "relay_site_ordinal": 1,
                    "occurrence_index": 0,
                },
            },
            {
                "status": "Passed", "check_kind": "certificate_must_precede",
                "reason": "ordered",
                "detail": {
                    "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                    "module_identity": "module", "function": "root",
                    "certificate_id": "pair.root.helper", "property_id": "pair.root.helper",
                    "certificate_relation": "MustPrecedeAB",
                    "site_a": "site", "site_b": "helper.site",
                },
            },
            {
                "status": "Passed", "check_kind": "certificate_coemission_independence",
                "reason": "independent",
                "detail": {
                    "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                    "module_identity": "module", "function": "root",
                    "certificate_id": "pair.root.helper.independent",
                    "property_id": "pair.root.helper.independent",
                    "certificate_relation": "CoEmissionIndependent",
                    "site_a": "site", "site_b": "helper.site",
                },
            },
        ])

        value = analyze_manifest("C", manifest, trace, ["root"])["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["observed"], 2)
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 2)
        self.assertEqual(value["relay_pairs"]["observed"], 1)
        self.assertEqual(value["relay_pairs"]["certified"], 1)
        self.assertEqual(len(value["relay_pairs"]["details"][0]["certificate_ids"]), 2)
        self.assertEqual(value["relay_pairs"]["details"][0]["certificate_owner_function_id"], "root")
        self.assertEqual(value["transactions"]["complete"], 1)

    def test_runtime_pass_cannot_upgrade_unknown_static_bound(self):
        manifest = minimal_manifest()
        direct = manifest["parallel_certificate"]["functions"][0]["direct_logical_work"]
        direct["status"] = "Unknown"
        direct["upper_bound"] = {"kind": "unknown", "reason": "loop occurrence unsupported"}
        value = analyze_manifest("C", manifest, self.covered_trace())["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["relay_emissions"]["details"][0]["direct_work_status"], "StaticUnknown")
        self.assertEqual(value["transactions"]["partial"], 1)

    def test_runtime_pass_requires_resolvable_static_evidence(self):
        manifest = minimal_manifest()
        direct = manifest["parallel_certificate"]["functions"][0]["direct_logical_work"]
        direct["supporting_cfg_fact_ids"] = ["missing.cfg.fact"]
        value = analyze_manifest("C", manifest, self.covered_trace())["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["relay_emissions"]["details"][0]["direct_work_status"], "InvalidEvidence")
        self.assertEqual(value["transactions"]["partial"], 1)

    def test_duplicate_pass_and_skip_cannot_collapse_to_pass(self):
        trace = self.covered_trace()
        trace["validation_results"].append({
            "status": "SkippedUnsupported", "check_kind": "certificate_direct_work",
            "reason": "ambiguous duplicate validation context",
            "detail": {
                "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                "module_identity": "module", "function": "root",
                "certificate_id": "work.direct", "property_id": "work.direct",
            },
        })
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(
            value["relay_emissions"]["details"][0]["direct_work_status"],
            "ConflictingValidation",
        )
        self.assertEqual(value["transactions"]["partial"], 1)

    def test_runtime_pass_for_wrong_certificate_id_is_not_coverage(self):
        trace = self.covered_trace()
        direct = next(
            value for value in trace["validation_results"]
            if value.get("check_kind") == "certificate_direct_work"
        )
        direct["detail"]["certificate_id"] = "different.certificate"
        direct["detail"]["property_id"] = "different.certificate"
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["relay_emissions"]["details"][0]["direct_work_status"], "Missing")
        self.assertEqual(value["transactions"]["partial"], 1)

    def test_binding_conflict_is_scoped_to_module_and_forces_fallback(self):
        trace = self.covered_trace()
        trace["validation_results"].append({
            "status": "ManifestBindingMismatch", "check_kind": "artifact_binding",
            "reason": "tampered manifest",
            "detail": {
                "root_trace_tx_id": 0, "current_trace_tx_id": 0,
                "module_identity": "module",
                "manifest_identity": {"binding_complete": True, "module_id": "module", "manifest_hash": "other"},
            },
        })
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["transactions"]["fallback"], 1)
        self.assertEqual(value["transactions"]["details"][0]["binding_statuses"]["module"], "ManifestBindingMismatch")

    def test_other_module_binding_pass_cannot_trust_selected_root(self):
        trace = self.covered_trace()
        binding = trace["validation_results"][0]
        binding["detail"]["module_identity"] = "other.module"
        binding["detail"]["manifest_identity"]["module_id"] = "other.module"
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["transactions"]["fallback"], 1)
        self.assertEqual(value["transactions"]["details"][0]["binding_statuses"]["module"], "Missing")

    def test_missing_emitted_child_execution_forces_fallback(self):
        trace = self.covered_trace()
        trace["relay_executions"] = [
            item for item in trace["relay_executions"] if bool(item.get("is_relay")) is False
        ]
        trace["counters"].update({"relay_executions": 0, "microtransactions_observed": 1})
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)
        self.assertEqual(value["transactions"]["fallback"], 1)
        self.assertIn(
            "relay emission child execution is missing",
            value["transactions"]["details"][0]["execution_integrity_errors"],
        )

    def test_duplicate_source_trace_ids_do_not_shrink_transaction_denominator(self):
        trace = self.covered_trace()
        trace["relay_executions"].append(copy.deepcopy(trace["relay_executions"][0]))
        trace["counters"].update({
            "source_transactions_observed": 2,
            "microtransactions_observed": 3,
        })
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["transactions"]["source_transactions"], 2)
        self.assertEqual(value["transactions"]["fallback"], 2)
        self.assertEqual(
            value["transactions"]["complete"]
            + value["transactions"]["partial"]
            + value["transactions"]["fallback"],
            value["transactions"]["source_transactions"],
        )

    def test_unresolved_root_identity_is_counted_as_fallback(self):
        trace = self.covered_trace()
        trace["relay_executions"][0]["source_function_id"] = ""
        value = analyze_manifest("C", minimal_manifest(), trace, ["root"])["runtime_weighted_coverage"]
        self.assertEqual(value["transactions"]["source_transactions"], 1)
        self.assertEqual(value["transactions"]["fallback"], 1)
        self.assertEqual(value["relay_emissions"]["observed"], 1)
        self.assertEqual(value["relay_emissions"]["certificate_covered"], 0)

    def test_unrelated_skipped_formula_check_does_not_demote_complete(self):
        trace = self.covered_trace()
        trace["validation_results"].append({
            "status": "SkippedUnsupported", "check_kind": "target_relation",
            "reason": "opaque formula",
            "detail": {
                "root_trace_tx_id": 1, "current_trace_tx_id": 1,
                "module_identity": "module", "function": "root",
            },
        })
        value = analyze_manifest("C", minimal_manifest(), trace)["runtime_weighted_coverage"]
        self.assertEqual(value["transactions"]["complete"], 1)

    def test_bound_coverage_requires_status_formula_and_evidence(self):
        unsupported = minimal_manifest()
        value = unsupported["parallel_certificate"]["functions"][0]["direct_logical_work"]
        value.update({"status": "Unsupported", "supporting_cfg_fact_ids": []})
        result = analyze_manifest("C", unsupported)
        self.assertEqual(result["protocol_coverage"]["direct_logical_work_bound"]["covered"], 0)
        direct_record = next(
            record for record in result["certificate_quality"]["records"]
            if record["property"] == "WorkBound"
            and record["subproperty"] == "DirectLogicalWork"
            and record["benchmark_function_id"] == "root"
        )
        self.assertEqual(direct_record["status"], "Unsupported")

        inconsistent = minimal_manifest()
        value = inconsistent["parallel_certificate"]["functions"][0]["direct_logical_work"]
        value.update({
            "status": "Complete",
            "exact": {"kind": "unknown", "reason": "opaque"},
            "upper_bound": {"kind": "unknown", "reason": "opaque"},
        })
        result = analyze_manifest("C", inconsistent)
        direct_record = next(
            record for record in result["certificate_quality"]["records"]
            if record["property"] == "WorkBound"
            and record["subproperty"] == "DirectLogicalWork"
            and record["benchmark_function_id"] == "root"
        )
        self.assertEqual(direct_record["status"], "Unknown")
        self.assertFalse(direct_record["finite_upper_bound"])
        self.assertEqual(result["protocol_coverage"]["direct_logical_work_bound"]["covered"], 0)

    def test_duplicate_refinement_semantic_key_is_rejected(self):
        manifest = minimal_manifest()
        duplicate = copy.deepcopy(manifest["refinement"]["proof_obligations"][0])
        duplicate["id"] = "target.obligation.duplicate"
        manifest["refinement"]["proof_obligations"].append(duplicate)
        with self.assertRaisesRegex(ManifestValidationError, "semantic keys"):
            validate_manifest(manifest)

        manifest = minimal_manifest()
        duplicate = copy.deepcopy(manifest["refinement"]["constraints"][2])
        duplicate["id"] = "nonnegative.root.duplicate"
        manifest["refinement"]["constraints"].append(duplicate)
        with self.assertRaisesRegex(ManifestValidationError, "semantic keys"):
            validate_manifest(manifest)

    def test_malformed_formula_and_unknown_symbol_are_rejected(self):
        manifest = minimal_manifest()
        manifest["refinement"]["proof_obligations"][0]["goal"] = {
            "kind": "Binary", "sort": {"kind": "Bool"},
            "operator": "==", "children": [],
        }
        with self.assertRaisesRegex(ManifestValidationError, "requires 2 children"):
            validate_manifest(manifest)

        manifest = minimal_manifest()
        manifest["refinement"]["proof_obligations"][0]["goal"] = {
            "kind": "Symbol", "sort": {"kind": "Bool"},
            "symbol_id": "missing.symbol", "operator": "", "children": [],
        }
        with self.assertRaisesRegex(ManifestValidationError, "unknown symbol_id"):
            validate_manifest(manifest)

    def test_parallel_pair_cross_references_are_rejected(self):
        manifest = minimal_manifest()
        manifest["parallel_certificate"]["functions"][0]["pair_relations"].append({
            "certificate_id": "bogus.pair", "source_function_id": "wrong.owner",
            "site_a": "ghost.a", "site_b": "ghost.b", "relation": "Unknown",
            "status": "Unsupported", "reason": "unsupported formula",
            "supporting_cfg_fact_ids": [], "supporting_constraint_ids": [],
            "supporting_solver_result_ids": [],
        })
        with self.assertRaises(ManifestValidationError) as captured:
            validate_manifest(manifest)
        message = str(captured.exception)
        self.assertIn("does not match certificate owner", message)
        self.assertIn("unknown relay site", message)


if __name__ == "__main__":
    unittest.main()
