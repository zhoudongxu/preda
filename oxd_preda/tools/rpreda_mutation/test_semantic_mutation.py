#!/usr/bin/env python3

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import SemanticMutationRunner as runner  # noqa: E402
from MutationRunner import CompileResult, ProcessResult  # noqa: E402


def site(
    site_id: str,
    function_id: str,
    signature: str,
) -> dict:
    return {
        "id": site_id,
        "source_function_id": function_id,
        "source_function_signature": signature,
        "source_scope": "address",
        "relay_kind": "custom_scope",
        "target_scope": "uint32",
        "target": {"kind": "identifier", "text": "target", "children": []},
        "arguments": [],
        "branches": [],
        "loops": [],
        "location": {"start_offset": 1, "end_offset": 2},
    }


def manifest(*sites: dict) -> dict:
    return {
        "contract": "chsimu.Test",
        "relay_sites": list(sites),
        "handlers": [],
        "refinement": {"constraints": [], "proof_obligations": []},
        "parallel_certificate": {"functions": []},
    }


class SemanticMutationRunnerTests(unittest.TestCase):
    def test_load_configuration_supports_multiple_benchmarks(self) -> None:
        payload = {
            "schema_version": 1,
            "benchmarks": {
                "One": {
                    "source": "One.prd",
                    "runtime_template": "One.prdts.in",
                    "mutation_kinds": ["RelayDuplicate"],
                },
                "Two": {
                    "source": "Two.prd",
                    "runtime_template": "Two.prdts.in",
                    "runtime_faults": [{"kind": "RuntimeRelayDuplicate"}],
                },
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "benchmarks.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            loaded = runner.load_configuration(path)
        self.assertEqual(set(loaded["benchmarks"]), {"One", "Two"})

    def test_load_configuration_requires_explicit_kinds_or_faults(self) -> None:
        payload = {
            "schema_version": 1,
            "benchmarks": {
                "Bad": {
                    "source": "Bad.prd",
                    "runtime_template": "Bad.prdts.in",
                }
            },
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "benchmarks.json"
            path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaises(runner.ConfigurationError):
                runner.load_configuration(path)

    def test_coverage_uses_portable_signatures(self) -> None:
        baseline = manifest(
            site(
                "relay_site_0",
                "chsimu.Token::transfer(address,bigint)",
                "transfer(address,bigint)",
            ),
            site(
                "relay_site_1",
                "chsimu.Token::transfer_n(array chsimu.Token.payment)",
                "transfer_n(array chsimu.Token.payment)",
            ),
        )
        benchmark = {
            "coverage_allowlist": {
                "portable_source_function_signatures": [
                    "transfer(address,bigint)"
                ]
            }
        }
        requirements = runner.coverage_requirements(
            baseline, "Token", benchmark
        )
        self.assertEqual(requirements["required_site_ids"], ["relay_site_0"])
        self.assertEqual(
            requirements["required_function_ids"],
            ["chsimu.Token::transfer(address,bigint)"],
        )

    def test_per_kind_filter_overrides_generic_and_caps_selection(self) -> None:
        benchmark = {
            "mutation_kinds": [
                "ArgumentArithmeticPerturb",
                "RelayDuplicate",
            ],
            "coverage_allowlist": {
                "portable_source_function_signatures": ["Token::transfer"]
            },
            "mutation_function_filters": {
                "ArgumentArithmeticPerturb": ["Token::transfer_n"],
                "RelayDuplicate": ["Token::transfer"],
            },
        }
        index = {
            "diagnostics": [],
            "mutations": [
                {
                    "mutation_id": "arg-transfer",
                    "mutation_type": "ArgumentArithmeticPerturb",
                    "generation_status": "Generated",
                    "source_function_id": "chsimu.Token::transfer(address,bigint)",
                },
                {
                    "mutation_id": "arg-airdrop",
                    "mutation_type": "ArgumentArithmeticPerturb",
                    "generation_status": "Generated",
                    "source_function_id": "chsimu.Token::transfer_n(array payment)",
                },
                {
                    "mutation_id": "dup-first",
                    "mutation_type": "RelayDuplicate",
                    "generation_status": "Generated",
                    "source_function_id": "chsimu.Token::transfer(address,bigint)",
                },
                {
                    "mutation_id": "dup-second",
                    "mutation_type": "RelayDuplicate",
                    "generation_status": "Generated",
                    "source_function_id": "chsimu.Token::transfer_n(array payment)",
                },
            ],
        }
        selected, missing = runner.select_generated_mutations(index, benchmark)
        self.assertFalse(missing)
        self.assertEqual(
            [row["mutation_id"] for row in selected],
            ["arg-airdrop", "dup-first"],
        )

    def test_root_default_can_raise_per_kind_selection_cap(self) -> None:
        benchmark = {
            "mutation_kinds": ["RelayDuplicate"],
            "coverage_allowlist": {
                "portable_source_function_signatures": ["Test::send"]
            },
        }
        index = {
            "diagnostics": [],
            "mutations": [
                {
                    "mutation_id": mutation_id,
                    "mutation_type": "RelayDuplicate",
                    "generation_status": "Generated",
                    "source_function_id": "chsimu.Test::send()",
                }
                for mutation_id in ("first", "second")
            ],
        }
        selected, missing = runner.select_generated_mutations(
            index, benchmark, {"max_selected_per_kind": 2}
        )
        self.assertFalse(missing)
        self.assertEqual(
            [row["mutation_id"] for row in selected], ["first", "second"]
        )

    def test_top_level_generation_diagnostics_fail_closed(self) -> None:
        with self.assertRaises(runner.GenerationDiagnosticsError) as caught:
            runner.validate_generation_index(
                {"diagnostics": ["edit rejected"], "mutations": []}
            )
        self.assertIn("edit rejected", str(caught.exception))

    def test_gas_used_up_is_infrastructure_not_runtime_kill(self) -> None:
        evidence = runner.runtime_detection_evidence(
            {
                "status": "RuntimeStrictMismatch",
                "termination_reason": "GasUsedUp",
                "runtime_mismatches": ["direct_count"],
                "certificate_mismatches": ["work"],
            }
        )
        self.assertTrue(evidence["infrastructure"])
        self.assertFalse(evidence["runtime"])
        self.assertFalse(evidence["certificate"])

    def test_runtime_fixture_compile_rejection_is_not_silently_passed(self) -> None:
        runtime = {
            "status": "CompilerRejected",
            "reason": "runtime fixture compilation rejected the source",
        }
        evidence = runner.runtime_detection_evidence(runtime)
        self.assertTrue(evidence["compiler_rejected"])
        self.assertFalse(evidence["infrastructure"])
        process = ProcessResult([], 0, False, 0.0, "", "")
        compiled = CompileResult(
            "Compiled", "", process, None, manifest()
        )
        control = runner._baseline_control("Synthetic", compiled, runtime)
        self.assertEqual(control["final_classification"], "CompilerRejected")

    def test_baseline_z3_encoding_limit_is_reported_without_blocking_other_layers(self) -> None:
        baseline = manifest()
        baseline["refinement"]["proof_obligations"] = [
            {
                "id": "count.upper_bound",
                "kind": "RelayCountUpperBound",
                "proof_role": "SolverGoal",
                "solver_result": {
                    "backend": "z3",
                    "status": "EncodingError",
                    "reason": "array container sort is unsupported",
                },
            }
        ]
        process = ProcessResult([], 0, False, 0.0, "", "")
        compiled = CompileResult("Compiled", "", process, None, baseline)
        runtime = {
            "status": "Passed",
            "termination_reason": "Completed",
            "runtime_mismatches": [],
            "certificate_mismatches": [],
            "infrastructure_failures": [],
        }
        control = runner._baseline_control("Kitty", compiled, runtime)
        self.assertEqual(control["final_classification"], "Survived")
        self.assertFalse(control["analysis_complete"])
        self.assertEqual(control["stage_failures"], ["Z3Baseline"])
        self.assertIn("baseline Z3 analysis incomplete", control["reason"])

    def test_runtime_and_certificate_mismatches_are_independent(self) -> None:
        evidence = runner.runtime_detection_evidence(
            {
                "status": "CertificateViolation",
                "termination_reason": "StrictValidationFailure",
                "runtime_mismatches": ["target_scope_kind"],
                "certificate_mismatches": ["direct_work"],
            }
        )
        self.assertTrue(evidence["runtime"])
        self.assertTrue(evidence["certificate"])
        self.assertFalse(evidence["infrastructure"])

    def test_fault_spec_resolves_portable_signature(self) -> None:
        baseline = manifest(
            site(
                "relay_site_0",
                "chsimu.MillionPixel::occupy(uint16,uint16)",
                "occupy(uint16,uint16)",
            )
        )
        raw = {
            "kind": "RuntimeTargetScopeCorruption",
            "selector": {
                "source_function_signature": "occupy(uint16,uint16)",
                "occurrence_index": 0,
            },
            "replacement_scope": "uint64",
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "fault.json"
            spec, error = runner.materialize_fault_spec(
                "MillionPixel", raw, baseline, 88, path
            )
            saved = json.loads(path.read_text(encoding="utf-8"))
        self.assertFalse(error)
        self.assertEqual(spec, saved)
        self.assertEqual(
            spec["selector"]["source_function_id"],
            "chsimu.MillionPixel::occupy(uint16,uint16)",
        )
        self.assertEqual(spec["selector"]["relay_site_id"], "relay_site_0")

    def test_fault_application_notapplicable_record_is_not_unsupported(self) -> None:
        report = {
            "validation_results": [
                {
                    "status": "NotApplicable",
                    "detail": {
                        "property_id": "runtime_fault.fault-1",
                        "actual": "Applied",
                        "relay_site_id": "relay_site_0",
                    },
                }
            ]
        }
        application = runner.inspect_fault_application(report, "fault-1")
        self.assertEqual(application["status"], "Applied")

    def test_fault_record_requires_application_expected_mismatch_and_rc2(self) -> None:
        raw = {
            "kind": "RuntimeRelayDuplicate",
            "expected_runtime_check_kinds": [
                "direct_count",
                "certificate_direct_work",
            ],
        }
        spec = {
            "mutation_id": "fault-dup",
            "kind": "RuntimeRelayDuplicate",
            "selector": {
                "source_function_id": "chsimu.Test::send()",
                "relay_site_id": "relay_site_0",
            },
        }
        report = {
            "validation_results": [
                {
                    "status": "NotApplicable",
                    "detail": {
                        "property_id": "runtime_fault.fault-dup",
                        "actual": "Applied",
                    },
                },
                {
                    "status": "Mismatch",
                    "check_kind": "direct_count",
                    "detail": {"check_kind": "direct_count"},
                },
                {
                    "status": "Mismatch",
                    "check_kind": "certificate_direct_work",
                    "detail": {
                        "check_kind": "certificate_direct_work",
                        "certificate_id": "work.bound",
                    },
                },
            ]
        }
        runtime = {
            "status": "CertificateViolation",
            "termination_reason": "StrictValidationFailure",
            "process_returncode": 2,
            "coverage": {"covered": True},
            "certificate_mismatches": ["work.bound"],
            "runtime_mismatches": ["direct_count"],
            "infrastructure_failures": [],
        }
        with tempfile.TemporaryDirectory() as directory:
            fault_path = pathlib.Path(directory) / "fault.json"
            fault_path.write_text("{}", encoding="utf-8")
            record = runner.build_fault_record(
                "Synthetic",
                raw,
                spec,
                fault_path,
                runtime,
                report,
                pathlib.Path(directory),
            )
        self.assertTrue(record["semantic_runtime_detected"])
        self.assertEqual(record["detection_layers"], ["certificate", "runtime"])
        self.assertIn("CertificateViolation", record["detected_by"])
        self.assertIn("RuntimeStrictMismatch", record["detected_by"])

    def test_fault_timeout_never_becomes_semantic_kill(self) -> None:
        raw = {"kind": "RuntimeTargetScopeCorruption"}
        spec = {
            "mutation_id": "fault-timeout",
            "kind": "RuntimeTargetScopeCorruption",
            "selector": {
                "source_function_id": "chsimu.Test::send()",
                "relay_site_id": "relay_site_0",
            },
        }
        runtime = {
            "status": "InfrastructureFailure",
            "termination_reason": "Timeout",
            "reason": "runtime timed out",
            "coverage": {"covered": False},
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "fault.json"
            path.write_text("{}", encoding="utf-8")
            record = runner.build_fault_record(
                "Synthetic", raw, spec, path, runtime, None, pathlib.Path(directory)
            )
        self.assertFalse(record["semantic_runtime_detected"])
        self.assertEqual(record["detection_layers"], [])
        self.assertEqual(record["final_classification"], "InfrastructureFailure")

    def test_source_record_calls_cross_version_comparator_once(self) -> None:
        baseline = manifest(
            site(
                "relay_site_0",
                "chsimu.Test::send(uint32)",
                "send(uint32)",
            )
        )
        mutant = json.loads(json.dumps(baseline))
        generated = {
            "mutation_id": "engine-id",
            "mutation_type": "TargetArithmeticPerturb",
            "generation_status": "Generated",
            "source_function_id": "chsimu.Test::send(uint32)",
            "relay_site_ids": ["relay_site_0"],
        }
        process = ProcessResult([], 0, False, 0.0, "", "")
        compiled = CompileResult("Compiled", "", process, None, mutant)
        refinement = {
            "applicable": True,
            "formula_ir_changed": True,
            "statuses": {"Disproved": 1},
            "obligations": [],
            "z3_disproved": ["goal"],
            "unsupported": [],
        }
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(
            runner, "compare_manifest_refinements", return_value=refinement
        ) as comparator, mock.patch.object(
            runner,
            "protocol_difference",
            return_value={"changed": False},
        ), mock.patch.object(
            runner, "certificate_regressions", return_value=[]
        ):
            record = runner.build_source_record(
                "Synthetic",
                generated,
                "aggregate-id",
                compiled,
                baseline,
                pathlib.Path("/z3"),
                None,
                pathlib.Path(directory),
            )
        comparator.assert_called_once()
        self.assertEqual(record["detection_layers"], ["z3"])
        self.assertEqual(record["final_classification"], "Z3Disproved")


if __name__ == "__main__":
    unittest.main()
