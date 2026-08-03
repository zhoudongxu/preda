#!/usr/bin/env python3

import copy
import json
import os
import pathlib
import tempfile
import unittest
from unittest import mock

import MutationReport
import MutationRunner


def manifest(targets=("a", "b")):
    sites = []
    for index, target in enumerate(targets):
        sites.append(
            {
                "id": f"relay_site_{index}",
                "source_function_id": "d.C::f(uint32,uint32)",
                "source_function_signature": "f(uint32,uint32)",
                "source_scope": "uint32",
                "relay_kind": "custom_scope",
                "target_scope": "uint32",
                "handler_id": "handler_0",
                "location": {"start_offset": 100 + index * 20},
                "target": {
                    "kind": "identifier",
                    "text": target,
                    "type": "uint32",
                    "operator": "primary",
                    "children": [],
                },
                "arguments": [],
                "branches": [],
                "loops": [],
            }
        )
    return {
        "contract": "d.C",
        "relay_sites": sites,
        "handlers": [
            {
                "id": "handler_0",
                "kind": "named",
                "name": "receive",
                "scope": "uint32",
                "parameter_types": [],
                "resolved": True,
                "may_emit_relay": False,
            }
        ],
        "refinement": {"proof_obligations": []},
        "parallel_certificate": {
            "functions": [
                {
                    "source_function_id": "d.C::f(uint32,uint32)",
                    "pair_relations": [
                        {
                            "site_a": "relay_site_0",
                            "site_b": "relay_site_1",
                            "relation": "CoEmissionIndependent",
                            "status": "Proved",
                        }
                    ],
                    "direct_logical_work": {
                        "upper_bound": {"kind": "constant", "value": 2}
                    },
                    "transitive_logical_work": {
                        "upper_bound": {"kind": "constant", "value": 2}
                    },
                    "relay_tree_depth": {
                        "upper_bound": {"kind": "constant", "value": 1}
                    },
                    "physical_route_work": {
                        "bound_kind": "Constant",
                        "constant_term": 2,
                        "active_shard_count_coefficient": 0,
                    },
                }
            ]
        },
    }


class ProjectionTests(unittest.TestCase):
    def test_projection_ignores_generated_ids_and_locations(self):
        left = manifest()
        right = copy.deepcopy(left)
        right["relay_sites"][0]["id"] = "renumbered"
        right["relay_sites"][0]["location"]["start_offset"] = 999
        right["handlers"][0]["id"] = "different_handler_id"
        for site in right["relay_sites"]:
            site["handler_id"] = "different_handler_id"
        self.assertFalse(MutationRunner.protocol_difference(left, right)["changed"])

    def test_projection_detects_target_and_order(self):
        baseline = manifest()
        target_changed = manifest(("a", "a"))
        self.assertTrue(
            MutationRunner.protocol_difference(baseline, target_changed)["changed"]
        )
        swapped = manifest(("b", "a"))
        self.assertTrue(MutationRunner.protocol_difference(baseline, swapped)["changed"])

    def test_certificate_regressions(self):
        baseline = manifest()
        mutant = copy.deepcopy(baseline)
        mutant["parallel_certificate"]["functions"][0]["pair_relations"] = []
        mutant["parallel_certificate"]["functions"][0]["direct_logical_work"][
            "upper_bound"
        ]["value"] = 3
        regressions = MutationRunner.certificate_regressions(baseline, mutant)
        self.assertTrue(any("lost proved" in item for item in regressions))
        self.assertTrue(any("bound increased" in item for item in regressions))

    def test_certificate_alignment_ignores_deleted_participant_and_renumbering(self):
        baseline = manifest(("a", "b", "c"))
        baseline_relations = baseline["parallel_certificate"]["functions"][0][
            "pair_relations"
        ]
        baseline_relations[0]["site_a"] = "relay_site_1"
        baseline_relations[0]["site_b"] = "relay_site_2"

        mutant = manifest(("b", "c"))
        mutant["relay_sites"][0]["location"]["start_offset"] = 120
        mutant["relay_sites"][1]["location"]["start_offset"] = 140
        generated = {
            "mutation_type": "RelayDelete",
            "relay_site_ids": ["relay_site_0"],
            "edits": [],
        }
        self.assertEqual(
            MutationRunner.certificate_regressions(
                baseline, mutant, generated
            ),
            [],
        )

    def test_order_swap_alignment_detects_semantic_precedence_reversal(self):
        first = "relay@a h();"
        second = "relay@b h();"
        original = first + "\n" + second + "\n"
        mutated = second + "\n" + first + "\n"
        second_start = len(first) + 1

        baseline = manifest(("a", "b"))
        mutant = manifest(("b", "a"))
        for value, starts in ((baseline, (0, second_start)), (mutant, (0, second_start))):
            for site, start in zip(value["relay_sites"], starts):
                site["location"] = {
                    "start_offset": start,
                    "end_offset": start + len(first) - 1,
                }
            value["parallel_certificate"]["functions"][0]["pair_relations"] = [
                {
                    "site_a": "relay_site_0",
                    "site_b": "relay_site_1",
                    "relation": "MustPrecedeAB",
                    "status": "Proved",
                }
            ]

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            original_path = root / "original.prd"
            mutant_path = root / "mutant.prd"
            original_path.write_text(original, encoding="utf-8")
            mutant_path.write_text(mutated, encoding="utf-8")
            generated = {
                "mutation_type": "RelayOrderSwap",
                "relay_site_ids": ["relay_site_0", "relay_site_1"],
                "original_code_path": str(original_path),
                "mutated_code_path": str(mutant_path),
                "edits": [
                    {
                        "byte_start_offset": 0,
                        "byte_end_offset": len(first) - 1,
                        "expected_original": first,
                        "replacement": second,
                    },
                    {
                        "byte_start_offset": second_start,
                        "byte_end_offset": second_start + len(second) - 1,
                        "expected_original": second,
                        "replacement": first,
                    },
                ],
            }
            regressions = MutationRunner.certificate_regressions(
                baseline, mutant, generated
            )
        self.assertTrue(any("precedence reversed" in value for value in regressions))


class ClassificationTests(unittest.TestCase):
    def test_z3_non_circular_classification(self):
        value = manifest()
        value["refinement"]["proof_obligations"] = [
            {
                "id": "mutual",
                "kind": "RelayMutualExclusion",
                "proof_role": "SolverGoal",
                "solver_result": {"backend": "z3", "status": "Disproved"},
            },
            {
                "id": "bound",
                "kind": "RelayCountUpperBound",
                "proof_role": "SolverGoal",
                "solver_result": {"backend": "z3", "status": "Proved"},
            },
            {
                "id": "definition",
                "kind": "RelayTargetEquality",
                "proof_role": "EstablishedByConstruction",
                "solver_result": {
                    "backend": "none",
                    "status": "EstablishedByConstruction",
                },
            },
        ]
        result = MutationRunner.inspect_z3(value, require_z3=True)
        self.assertEqual(result["safety_disproved"], [])
        self.assertEqual(result["infrastructure_failures"], [])
        value["refinement"]["proof_obligations"][1]["solver_result"][
            "status"
        ] = "Disproved"
        result = MutationRunner.inspect_z3(value, require_z3=True)
        self.assertEqual(result["safety_disproved"], ["bound"])

    def test_not_run_z3_is_infrastructure_when_required(self):
        value = manifest()
        value["refinement"]["proof_obligations"] = [
            {
                "id": "goal",
                "kind": "RelayCountUpperBound",
                "proof_role": "SolverGoal",
                "solver_result": {"backend": "none", "status": "NotRun"},
            }
        ]
        result = MutationRunner.inspect_z3(value, require_z3=True)
        self.assertTrue(result["infrastructure_failures"])

    def test_trace_categories(self):
        report = {
            "validation_results": [
                {"status": "Passed", "detail": {"check_kind": "binding"}},
                {
                    "status": "Mismatch",
                    "detail": {"certificate_id": "cert-1"},
                },
                {
                    "status": "Mismatch",
                    "detail": {"relay_site_id": "site-1"},
                },
                {
                    "status": "TraceInstrumentationError",
                    "detail": {"check_kind": "instrumentation"},
                },
            ]
        }
        result = MutationRunner.inspect_trace(report)
        self.assertEqual(result["certificate_mismatches"], ["cert-1"])
        self.assertEqual(result["runtime_mismatches"], ["site-1"])
        self.assertTrue(result["infrastructure_failures"])

    def test_controlled_runtime_fault_requires_provenance_and_expected_check(self):
        report = {
            "validation_results": [
                {
                    "status": "NotApplicable",
                    "check_kind": "unknown",
                    "detail": {
                        "property_id": "runtime_fault.fault-1",
                        "expected": "RuntimeTargetScopeCorruption",
                        "actual": "Applied",
                        "function": "d.C::f()",
                        "relay_site_id": "relay_site_0",
                        "occurrence_index": 0,
                    },
                },
                {
                    "status": "Mismatch",
                    "check_kind": "target_scope_kind",
                    "detail": {
                        "check_kind": "target_scope_kind",
                        "relay_site_id": "relay_site_0",
                    },
                },
            ]
        }
        inspection = MutationRunner.inspect_trace(report)
        result = MutationRunner.inspect_runtime_fault(
            inspection,
            {
                "mutation_id": "fault-1",
                "kind": "RuntimeTargetScopeCorruption",
            },
            2,
            False,
        )
        self.assertTrue(result["semantic_runtime_detected"])
        self.assertEqual(
            result["matching_mismatch_check_kinds"], ["target_scope_kind"]
        )
        self.assertEqual(inspection["skipped_unsupported"], 0)

        wrong_exit = MutationRunner.inspect_runtime_fault(
            inspection,
            {
                "mutation_id": "fault-1",
                "kind": "RuntimeTargetScopeCorruption",
            },
            0,
            False,
        )
        self.assertFalse(wrong_exit["semantic_runtime_detected"])

    def test_trace_coverage_gate_reports_missing_functions_and_sites(self):
        inspection = {
            "observed_function_ids": ["d.C::f()"],
            "observed_relay_site_ids": ["relay_site_0"],
        }
        coverage = MutationRunner.inspect_runtime_coverage(
            inspection,
            {
                "label": "m1",
                "required_function_ids": ["d.C::f()", "d.C::g()"],
                "required_site_ids": ["relay_site_0", "relay_site_1"],
                "unresolved_site_ids": ["original_site_9"],
            },
        )
        self.assertFalse(coverage["covered"])
        self.assertEqual(coverage["missing_function_ids"], ["d.C::g()"])
        self.assertEqual(coverage["missing_relay_site_ids"], ["relay_site_1"])
        self.assertEqual(coverage["unresolved_relay_site_ids"], ["original_site_9"])

    def test_mutation_coverage_tracks_changed_and_new_sites(self):
        baseline = manifest(("a", "b"))
        mutant = manifest(("a", "b", "a"))
        duplicate = {
            "mutation_id": "dup",
            "mutation_type": "RelayDuplicate",
            "source_function_id": "d.C::f(uint32,uint32)",
            "relay_site_ids": ["relay_site_0"],
        }
        requirements = MutationRunner.mutation_coverage_requirements(
            duplicate, baseline, mutant
        )
        self.assertEqual(
            requirements["required_site_ids"],
            ["relay_site_0", "relay_site_2"],
        )

        guard = dict(duplicate, mutation_type="GuardNegate", mutation_id="guard")
        requirements = MutationRunner.mutation_coverage_requirements(
            guard, baseline, baseline
        )
        self.assertEqual(requirements["required_site_ids"], ["relay_site_0"])

        deletion = dict(duplicate, mutation_type="RelayDelete", mutation_id="delete")
        requirements = MutationRunner.mutation_coverage_requirements(
            deletion, baseline, mutant
        )
        self.assertEqual(requirements["required_site_ids"], [])
        self.assertEqual(
            requirements["required_function_ids"],
            ["d.C::f(uint32,uint32)"],
        )

    def test_priority(self):
        self.assertEqual(
            MutationRunner.choose_classification(
                ["StaticProtocolMismatch", "InfrastructureFailure"], False
            ),
            "StaticProtocolMismatch",
        )
        self.assertEqual(
            MutationRunner.choose_classification(["InfrastructureFailure"], False),
            "InfrastructureFailure",
        )
        self.assertEqual(
            MutationRunner.choose_classification(
                ["StaticProtocolMismatch", "CertificateViolation"], False
            ),
            "StaticProtocolMismatch",
        )
        self.assertEqual(MutationRunner.choose_classification([], True), "Unsupported")
        self.assertEqual(MutationRunner.choose_classification([], False), "Survived")


class ProcessAndReportTests(unittest.TestCase):
    def test_exit_zero_compile_failure_is_rejected(self):
        if os.name == "nt":
            self.skipTest("shell fixture is Unix-specific")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            fake = root / "chsimu"
            fake.write_text(
                "#!/bin/sh\necho 'compile error #17: missing'\n"
                "echo '[PRD]: Compile failed'\nexit 0\n",
                encoding="utf-8",
            )
            fake.chmod(0o755)
            source = root / "input.prd"
            source.write_text("contract C {}\n", encoding="utf-8")
            result = MutationRunner.compile_source(
                source, root / "run", fake, [], [], 5.0
            )
            self.assertEqual(result.status, "CompilerRejected")

    def test_metrics_and_required_csv_fields(self):
        records = [
            {
                "mutation_id": "m1",
                "mutation_type": "RelayDelete",
                "final_classification": "StaticProtocolMismatch",
                "detected_by": [
                    "StaticProtocolMismatch",
                    "InfrastructureFailure",
                ],
            },
            {
                "mutation_id": "m2",
                "mutation_type": "RelayDelete",
                "final_classification": "Survived",
                "detected_by": [],
            },
            {
                "mutation_id": "m3",
                "mutation_type": "GuardNegate",
                "final_classification": "Unsupported",
                "detected_by": [],
                "generation_status": "Unsupported",
                "analysis": {
                    "z3": {
                        "unsupported": ["goal: Unsupported"],
                        "statuses": {"Unsupported": 1},
                    },
                    "runtime": {"skipped_unsupported": 2},
                },
            },
        ]
        controls = [
            {"final_classification": "Survived"},
            {"final_classification": "CompilerRejected"},
        ]
        metrics = MutationReport.calculate_metrics(records, controls)
        self.assertEqual(metrics["overall_detection_rate"], 1 / 3)
        self.assertEqual(metrics["supported_detection_rate"], 1 / 2)
        self.assertEqual(metrics["false_positive_rate"], 1 / 2)
        self.assertEqual(metrics["stage_infrastructure_failures"], 1)
        self.assertEqual(metrics["full_pipeline_completion_rate"], 2 / 3)
        self.assertEqual(metrics["fully_analyzable_mutants"], 1)
        self.assertEqual(
            metrics["per_stage_detection"]["StaticProtocolMismatch"][
                "detected_mutants"
            ],
            1,
        )
        self.assertEqual(
            metrics["unique_kill_counts"]["StaticProtocolMismatch"], 1
        )
        self.assertEqual(
            metrics["deeper_stage_score_excluding_static"]["detected_mutants"],
            0,
        )
        self.assertEqual(
            metrics["unsupported_breakdown"]["operator_inapplicable"]["mutants"],
            1,
        )
        self.assertEqual(
            metrics["unsupported_breakdown"]["z3"][
                "unsupported_obligations"
            ],
            1,
        )
        self.assertEqual(
            metrics["unsupported_breakdown"]["runtime"]["skipped_checks"],
            2,
        )
        self.assertEqual(metrics["original_control_flag_rate"], 1 / 2)
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary)
            MutationReport.write_results(output, {"seed": 88}, records, controls)
            payload = json.loads((output / "mutation.json").read_text())
            self.assertEqual(payload["schema_version"], 1)
            header = (output / "mutation.csv").read_text().splitlines()[0]
            for field in (
                "mutation_id",
                "mutation_type",
                "contract",
                "location",
                "detection_method",
                "final_classification",
            ):
                self.assertIn(field, header)

    def _run_fake_runtime(self, process, report, requirements):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = pathlib.Path(temporary.name)
        source = root / "input.prd"
        template = root / "runtime.prdts.in"
        chsimu = root / "chsimu"
        source.write_text("contract C {}\n", encoding="utf-8")
        template.write_text("chain.deploy @0 {{SOURCE}}\n", encoding="utf-8")
        chsimu.write_text("", encoding="utf-8")

        def fake_run(command, cwd, environment, timeout_seconds):
            del cwd, environment, timeout_seconds
            report_argument = next(
                value
                for value in command
                if value.startswith("-rpreda_trace_report:")
            )
            report_path = pathlib.Path(report_argument.split(":", 1)[1])
            report_path.write_text(json.dumps(report), encoding="utf-8")
            return process

        with mock.patch.object(MutationRunner, "run_process", side_effect=fake_run):
            return MutationRunner.run_runtime_strict(
                source,
                template,
                root / "run",
                chsimu,
                [],
                [],
                1.0,
                [],
                requirements,
                root,
            )

    def test_runtime_diagnostics_distinguish_gas_trace_and_coverage(self):
        report = {
            "logical_relay_emissions": [
                {
                    "source_function_id": "d.C::f()",
                    "relay_site_id": "relay_site_0",
                }
            ],
            "relay_executions": [],
            "validation_results": [],
        }
        requirements = {
            "required_function_ids": ["d.C::f()"],
            "required_site_ids": ["relay_site_0"],
        }
        gas_report = copy.deepcopy(report)
        gas_report["validation_results"] = [
            {
                "status": "TraceInstrumentationError",
                "detail": {"check_kind": "recurrence"},
            }
        ]
        gas = self._run_fake_runtime(
            MutationRunner.ProcessResult([], 2, False, 0.2, "GasUsedUp", ""),
            gas_report,
            requirements,
        )
        self.assertEqual(gas["status"], "RuntimeStrictMismatch")
        self.assertEqual(gas["termination_reason"], "GasUsedUp")
        self.assertTrue(gas["infrastructure_failures"])

        trace_error = self._run_fake_runtime(
            MutationRunner.ProcessResult([], 2, False, 0.2, "", ""),
            gas_report,
            requirements,
        )
        self.assertEqual(trace_error["status"], "InfrastructureFailure")
        self.assertEqual(
            trace_error["termination_reason"], "TraceInstrumentationError"
        )

        missing = self._run_fake_runtime(
            MutationRunner.ProcessResult([], 0, False, 0.2, "", ""),
            report,
            {
                "required_function_ids": ["d.C::f()"],
                "required_site_ids": ["relay_site_1"],
            },
        )
        self.assertEqual(missing["status"], "InfrastructureFailure")
        self.assertEqual(missing["termination_reason"], "CoverageFailure")

        timeout = self._run_fake_runtime(
            MutationRunner.ProcessResult([], -15, True, 1.0, "", ""),
            report,
            requirements,
        )
        self.assertEqual(timeout["status"], "InfrastructureFailure")
        self.assertEqual(timeout["termination_reason"], "Timeout")

    def test_unsupported_control_is_excluded_from_strict_fpr(self):
        controls = [{"final_classification": "Unsupported", "detected_by": []}]
        metrics = MutationReport.calculate_metrics([], controls)
        self.assertIsNone(metrics["false_positive_rate"])
        self.assertEqual(metrics["original_control_flag_rate"], 0.0)
        self.assertEqual(metrics["negative_control_unsupported"], 1)

    def test_portable_paths_and_persisted_logs_do_not_leak_home(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary)
            artifact = output / "runs" / "m1" / "trace.json"
            self.assertEqual(
                MutationRunner.portable_path(artifact, output),
                "runs/m1/trace.json",
            )
            self.assertEqual(
                MutationRunner.portable_path(artifact, output, retained=False),
                "not-retained/runs/m1/trace.json",
            )
            private = pathlib.Path.home() / "private" / "fixture.prd"
            self.assertEqual(
                MutationRunner.portable_path(private, output),
                "external/fixture.prd",
            )
            process = MutationRunner.ProcessResult(
                [str(private), f"-rpreda_trace_report:{artifact}"],
                0,
                False,
                0.1,
                f"compiled {private}",
                f"trace {artifact}",
            )
            MutationRunner.persist_process(output / "process", process, output)
            published = "\n".join(
                path.read_text(encoding="utf-8")
                for path in (output / "process").iterdir()
            )
            self.assertNotIn(str(pathlib.Path.home()), published)

    def test_generation_metadata_is_published_with_portable_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary)
            mutant_directory = output / "generation" / "mutants" / "m1"
            mutant_directory.mkdir(parents=True)
            metadata_path = mutant_directory / "mutation.json"
            metadata_path.write_text("{}\n", encoding="utf-8")
            original = pathlib.Path.home() / "private" / "original.prd"
            mutated = mutant_directory / "mutant.prd"
            index = {
                "source_path": str(original),
                "manifest_path": str(output / "baseline" / "oracle.json"),
                "mutations": [
                    {
                        "mutation_id": "m1",
                        "original_code_path": str(original),
                        "mutated_code_path": str(mutated),
                    }
                ],
            }
            index_path = output / "generation" / "mutation_index.json"
            MutationRunner.publish_generation_metadata(index, index_path, output)
            published = index_path.read_text(encoding="utf-8")
            per_mutant = metadata_path.read_text(encoding="utf-8")
            self.assertNotIn(str(pathlib.Path.home()), published + per_mutant)
            value = json.loads(published)
            self.assertEqual(value["source_path"], "external/original.prd")
            self.assertEqual(
                value["mutations"][0]["mutated_code_path"],
                "generation/mutants/m1/mutant.prd",
            )


if __name__ == "__main__":
    unittest.main()
