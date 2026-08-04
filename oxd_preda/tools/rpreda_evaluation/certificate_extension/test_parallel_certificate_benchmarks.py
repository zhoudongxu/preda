#!/usr/bin/env python3
"""Focused contract tests for the parallel-certificate benchmark runner.

The five workloads extend the four existing PREDA contract sources directly:
Token and AirDrop intentionally share ``Token.prd``.  These tests exercise the
runner's pure helper API; process-level compile/runtime coverage belongs to the
integration benchmark run.
"""

import copy
import csv
import json
import os
import pathlib
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from ParallelCertificateBenchmarkRunner import (
    BenchmarkError,
    CSV_FIELDS,
    certificate_function,
    evaluate_certificate,
    expected_regression_matches,
    finite_upper_bound,
    load_configuration,
    load_json,
    mutation_kinds,
    redactor,
    resolve_site,
    resolve_sites,
    select_mutation,
    validate_configuration,
    validate_manifest,
    write_csv,
    write_json,
)
from test_evaluation import formula, minimal_manifest


REQUIRED_RECORD_FIELDS = {
    "workload",
    "pattern",
    "certificate_type",
    "relay_site_a",
    "relay_site_b",
    "status",
    "evidence_ids",
    "mutation_mapping",
}

EXPECTED_WORKLOADS = {
    "TokenParallel",
    "BallotParallel",
    "MillionPixelParallel",
    "KittyParallel",
    "AirDropParallel",
}

EXPECTED_ORIGINAL_SOURCES = {
    "oxd_preda/simulator/contracts/Token.prd",
    "oxd_preda/simulator/contracts/Ballot.prd",
    "oxd_preda/simulator/contracts/MillionPixel.prd",
    "oxd_preda/simulator/contracts/Kitty.prd",
}


def _configuration_path():
    override = os.environ.get("RPREDA_PARALLEL_BENCHMARK_CONFIG", "")
    candidates = [
        pathlib.Path(override) if override else None,
        pathlib.Path(__file__).with_name("parallel_certificate_benchmarks.json"),
        pathlib.Path("/tmp/rpreda_extension_assets/parallel_certificate_benchmarks.json"),
    ]
    for candidate in candidates:
        if candidate is not None and candidate.is_file():
            return candidate
    raise AssertionError("parallel-certificate benchmark configuration is missing")


def _add_second_site(manifest):
    """Extend ``minimal_manifest`` with a second, linked relay site."""

    first = manifest["relay_sites"][0]
    first.update(
        {
            "target_function": "handler_a",
            "target": {"text": "first", "type": "uint32"},
        }
    )
    second = copy.deepcopy(first)
    second.update(
        {
            "id": "site_b",
            "handler_id": "handler_b",
            "target_function": "handler_b",
            "target": {"text": "second", "type": "uint32"},
        }
    )
    manifest["relay_sites"].append(second)
    manifest["handlers"].append(
        {
            "id": "handler_b",
            "resolved": True,
            "target_function_id": "leaf",
            "kind": "named",
        }
    )
    manifest["edges"].append(
        {
            "id": "edge_b",
            "source_function_id": "root",
            "relay_site_id": "site_b",
            "handler_id": "handler_b",
            "resolved": True,
        }
    )
    manifest["functions"][0]["relay_site_ids"].append("site_b")
    manifest["control_flow"]["functions"][0]["nodes"].append(
        {"id": "cfg.site_b", "kind": "RelayEmit", "relay_site_id": "site_b"}
    )

    first_constraint = manifest["refinement"]["constraints"][0]
    first_constraint["relay_site_id"] = "site"
    manifest["refinement"]["constraints"].append(
        {
            "id": "target.constraint.b",
            "kind": "RelayTargetRelation",
            "source_function_id": "root",
            "relay_site_id": "site_b",
            "formula": formula(),
        }
    )
    return manifest


def _manifest_with_two_sites():
    manifest = _add_second_site(minimal_manifest())
    validate_manifest(manifest)
    return manifest


def _solver_obligation(identifier, kind, status):
    return {
        "id": identifier,
        "kind": kind,
        "status": "Generated",
        "source_function_id": "root",
        "relay_site_id": "site",
        "related_relay_site_id": "site_b",
        "constraint_ids": (
            ["target.constraint", "target.constraint.b"]
            if kind == "RelayTargetIndependence"
            else []
        ),
        "goal": formula(),
        "solver_result": {
            "backend": "z3",
            "status": status,
            "elapsed_time_ms": 1,
            "assumption_constraint_ids": [],
            "reason": "fixture",
            "projected_counterexample": [],
        },
    }


def _pair(
    relation,
    certificate_id,
    *,
    solver_ids=(),
    constraint_ids=(),
    cfg_ids=("root", "cfg.site_b"),
    co_emission_status="NotRun",
    relation_status="NotRun",
):
    return {
        "certificate_id": certificate_id,
        "source_function_id": "root",
        "site_a": "site",
        "site_b": "site_b",
        "relation": relation,
        "status": "Proved",
        "reason": "fixture",
        "supporting_cfg_fact_ids": list(cfg_ids),
        "supporting_constraint_ids": list(constraint_ids),
        "supporting_solver_result_ids": list(solver_ids),
        "solver_results": {
            "co_emission": {"status": co_emission_status},
            "relation": {"status": relation_status},
        },
    }


def _benchmark():
    return {"name": "Fixture", "function_signature": "root(uint32)"}


def _expectation(certificate_type, relation):
    return {
        "id": "fixture." + certificate_type,
        "pattern": "fixture-pattern",
        "certificate_type": certificate_type,
        "site_a": "left",
        "site_b": "right",
        "expected_relation": relation,
        "expected_status": "Proved",
        "mutation": {},
    }


def _evaluate(manifest, expectation):
    sites = {"left": manifest["relay_sites"][0], "right": manifest["relay_sites"][1]}
    return evaluate_certificate(
        _benchmark(),
        expectation,
        "root",
        sites,
        certificate_function(manifest, "root"),
    )


def _install_mutual_exclusion(manifest, solver_status="Proved"):
    manifest["refinement"]["proof_obligations"].append(
        _solver_obligation("proof.mutual", "RelayMutualExclusion", solver_status)
    )
    manifest["parallel_certificate"]["functions"][0]["pair_relations"] = [
        _pair(
            "MutuallyExclusive",
            "certificate.mutual",
            solver_ids=["proof.mutual"],
            co_emission_status=solver_status,
        )
    ]
    return manifest


def _install_independence(
    manifest, feasibility_status="Disproved", independence_status="Proved"
):
    manifest["refinement"]["proof_obligations"].extend(
        [
            _solver_obligation(
                "proof.feasibility", "RelayMutualExclusion", feasibility_status
            ),
            _solver_obligation(
                "proof.independence",
                "RelayTargetIndependence",
                independence_status,
            ),
        ]
    )
    manifest["parallel_certificate"]["functions"][0]["pair_relations"] = [
        _pair(
            "CoEmissionIndependent",
            "certificate.independent",
            solver_ids=["proof.feasibility", "proof.independence"],
            constraint_ids=["target.constraint", "target.constraint.b"],
            co_emission_status=feasibility_status,
            relation_status=independence_status,
        )
    ]
    return manifest


class DynamicLocatorTests(unittest.TestCase):
    def test_dynamic_locator_resolves_exactly_one_site(self):
        manifest = _manifest_with_two_sites()
        site = resolve_site(
            manifest,
            {"target_function": "handler_a", "target_text": "first"},
            "root(uint32)",
        )
        self.assertEqual(site["id"], "site")

    def test_dynamic_locator_rejects_zero_and_multiple_matches(self):
        manifest = _manifest_with_two_sites()
        with self.assertRaises(BenchmarkError):
            resolve_site(
                manifest,
                {"target_function": "missing"},
                "root(uint32)",
            )

        manifest["relay_sites"][1]["target_function"] = "handler_a"
        with self.assertRaises(BenchmarkError):
            resolve_site(
                manifest,
                {"target_function": "handler_a"},
                "root(uint32)",
            )

    def test_occurrence_is_explicit_and_labels_still_resolve_one_to_one(self):
        manifest = _manifest_with_two_sites()
        manifest["relay_sites"][1]["target_function"] = "handler_a"
        selected = resolve_site(
            manifest,
            {"target_function": "handler_a", "occurrence": 1},
            "root(uint32)",
        )
        self.assertEqual(selected["id"], "site_b")

        benchmark = {
            "function_signature": "root(uint32)",
            "sites": {
                "left": {"target_function": "handler_a", "occurrence": 0},
                "right": {"target_function": "handler_a", "occurrence": 0},
            },
        }
        with self.assertRaises(BenchmarkError):
            resolve_sites(manifest, benchmark)


class PairEvidenceGateTests(unittest.TestCase):
    def test_mutual_exclusion_requires_matching_proved_solver_obligation(self):
        valid = _evaluate(
            _install_mutual_exclusion(_manifest_with_two_sites()),
            _expectation("MutualExclusive", "MutuallyExclusive"),
        )
        self.assertEqual(valid["status"], "Proved")
        self.assertEqual(valid["validation_status"], "Passed")
        self.assertIn("proof.mutual", valid["evidence_ids"])

        wrong_status = _evaluate(
            _install_mutual_exclusion(
                _manifest_with_two_sites(), solver_status="Disproved"
            ),
            _expectation("MutualExclusive", "MutuallyExclusive"),
        )
        self.assertEqual(wrong_status["validation_status"], "Failed")

    def test_independence_requires_solver_results_and_both_target_relations(self):
        manifest = _install_independence(_manifest_with_two_sites())
        valid = _evaluate(
            manifest,
            _expectation("CoEmissionIndependent", "CoEmissionIndependent"),
        )
        self.assertEqual(valid["status"], "Proved")
        self.assertEqual(valid["validation_status"], "Passed")
        self.assertTrue(
            {"proof.feasibility", "proof.independence"}.issubset(
                valid["evidence_ids"]
            )
        )

        missing_target_relation = copy.deepcopy(manifest)
        pair = missing_target_relation["parallel_certificate"]["functions"][0][
            "pair_relations"
        ][0]
        pair["supporting_constraint_ids"] = ["target.constraint"]
        invalid = _evaluate(
            missing_target_relation,
            _expectation("CoEmissionIndependent", "CoEmissionIndependent"),
        )
        self.assertEqual(invalid["validation_status"], "Failed")

    def test_precedence_requires_cfg_emit_facts_and_expected_direction(self):
        manifest = _manifest_with_two_sites()
        manifest["parallel_certificate"]["functions"][0]["pair_relations"] = [
            _pair("MustPrecedeAB", "certificate.precedence")
        ]
        valid = _evaluate(
            manifest, _expectation("MustPrecede", "MustPrecedeAB")
        )
        self.assertEqual(valid["validation_status"], "Passed")

        missing_emit_fact = copy.deepcopy(manifest)
        missing_emit_fact["parallel_certificate"]["functions"][0][
            "pair_relations"
        ][0]["supporting_cfg_fact_ids"] = ["root"]
        invalid = _evaluate(
            missing_emit_fact, _expectation("MustPrecede", "MustPrecedeAB")
        )
        self.assertEqual(invalid["validation_status"], "Failed")

        reversed_expectation = _expectation("MustPrecede", "MustPrecedeBA")
        reversed_result = _evaluate(manifest, reversed_expectation)
        self.assertEqual(reversed_result["validation_status"], "Failed")


class BoundAndMutationTests(unittest.TestCase):
    def test_finite_bound_extraction_and_status_validation(self):
        manifest = _manifest_with_two_sites()
        function = certificate_function(manifest, "root")
        self.assertEqual(finite_upper_bound(function["direct_logical_work"]), 1)
        self.assertEqual(finite_upper_bound(function["relay_tree_depth"]), 1)
        self.assertEqual(finite_upper_bound(function["physical_route_work"]), 1)

        unknown = copy.deepcopy(function["direct_logical_work"])
        unknown["upper_bound"] = {
            "kind": "unknown",
            "reason": "loop occurrence unsupported",
        }
        self.assertIsNone(finite_upper_bound(unknown))

        physical_unknown = copy.deepcopy(function["physical_route_work"])
        physical_unknown["bound_kind"] = "Unknown"
        self.assertIsNone(finite_upper_bound(physical_unknown))

        expectation = {
            "id": "fixture.work",
            "pattern": "fixture-pattern",
            "certificate_type": "WorkBound",
            "bound_field": "direct_logical_work",
            "expected_upper_bound": 1,
            "expected_status": "Proved",
            "mutation": {},
        }
        sites = {"left": manifest["relay_sites"][0], "right": manifest["relay_sites"][1]}
        valid = evaluate_certificate(_benchmark(), expectation, "root", sites, function)
        self.assertEqual(valid["validation_status"], "Passed")

        unsupported = copy.deepcopy(function)
        unsupported["direct_logical_work"]["status"] = "Unsupported"
        invalid = evaluate_certificate(
            _benchmark(), expectation, "root", sites, unsupported
        )
        self.assertEqual(invalid["validation_status"], "Failed")

    def test_mutation_selector_and_regression_mapping(self):
        manifest = _manifest_with_two_sites()
        sites = {"left": manifest["relay_sites"][0], "right": manifest["relay_sites"][1]}
        expectation = {
            "id": "fixture.independence",
            "mutation": {
                "kind": "IntroduceAlias",
                "sites": ["left", "right"],
                "expected_regression_contains": "lost proved CoEmissionIndependent",
            },
        }
        index = {
            "mutations": [
                {
                    "mutation_id": "mut.alias",
                    "mutation_type": "IntroduceAlias",
                    "generation_status": "Generated",
                    "source_function_id": "root",
                    "relay_site_ids": ["site", "site_b"],
                }
            ]
        }
        selected = select_mutation(index, expectation, "root", sites)
        self.assertEqual(selected["mutation_id"], "mut.alias")
        self.assertTrue(
            expected_regression_matches(
                ["fixture: lost proved CoEmissionIndependent(site,site_b)"],
                expectation["mutation"],
            )
        )
        self.assertFalse(
            expected_regression_matches(
                ["fixture: unrelated regression"], expectation["mutation"]
            )
        )

        duplicate = copy.deepcopy(index)
        duplicate["mutations"].append(copy.deepcopy(index["mutations"][0]))
        with self.assertRaises(BenchmarkError):
            select_mutation(duplicate, expectation, "root", sites)


class ResultAndConfigurationTests(unittest.TestCase):
    def _record(self):
        return {
            "record_id": "token.receiver_audit.independence",
            "workload": "TokenParallel",
            "pattern": "receiver_audit",
            "certificate_type": "CoEmissionIndependent",
            "source_function_signature": "transfer_parallel(address,address,bigint)",
            "relay_site_a": "site",
            "relay_site_b": "site_b",
            "status": "Proved",
            "raw_certificate_status": "Proved",
            "validation_status": "Passed",
            "evidence_ids": ["root", "proof.independence"],
            "reason": "fixture",
            "mutation_mapping": {
                "kind": "IntroduceAlias",
                "validation_required": True,
            },
            "mutation": {
                "mutation_id": "mut.alias",
                "mutation_type": "IntroduceAlias",
                "generation_status": "Generated",
                "compile_status": "Compiled",
                "runtime_status": "Passed",
                "detected": True,
                "regressions": ["lost proved CoEmissionIndependent"],
            },
        }

    def test_json_csv_have_required_fields_and_no_private_path(self):
        with tempfile.TemporaryDirectory() as raw:
            output = pathlib.Path(raw)
            config = _configuration_path()
            sanitize = redactor(
                pathlib.Path("/workspace/preda"), output, config, []
            )
            payload = sanitize(
                {
                    "schema_version": 1,
                    "metadata": {
                        "runner": "parallel-certificate-extension",
                        "work_root": str(pathlib.Path.home() / "private" / "run"),
                    },
                    "summary": {"workloads": 1, "records": 1},
                    "workloads": [{"workload": "TokenParallel"}],
                    "records": [self._record()],
                }
            )
            json_path = output / "certificate_extension.json"
            csv_path = output / "certificate_extension.csv"
            write_json(json_path, payload)
            write_csv(csv_path, payload["records"])

            parsed = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertTrue(REQUIRED_RECORD_FIELDS.issubset(parsed["records"][0]))
            with csv_path.open(encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertTrue(REQUIRED_RECORD_FIELDS.issubset(rows[0]))
            self.assertEqual(
                json.loads(rows[0]["evidence_ids"]),
                ["root", "proof.independence"],
            )
            self.assertIsInstance(json.loads(rows[0]["mutation_mapping"]), dict)
            self.assertTrue(REQUIRED_RECORD_FIELDS.issubset(CSV_FIELDS))

            serialized = json_path.read_text(encoding="utf-8") + csv_path.read_text(
                encoding="utf-8"
            )
            self.assertNotIn("/home/", serialized)
            self.assertIn("<home>/private/run", serialized)

    def test_config_uses_five_entries_but_only_four_original_sources(self):
        config_path = _configuration_path()
        raw = load_json(config_path)
        validate_configuration(raw)
        benchmarks = raw["benchmarks"]
        self.assertEqual({str(value["name"]) for value in benchmarks}, EXPECTED_WORKLOADS)
        self.assertEqual({str(value["source"]) for value in benchmarks}, EXPECTED_ORIGINAL_SOURCES)
        self.assertEqual(len(benchmarks), 5)
        self.assertEqual(len({value["source"] for value in benchmarks}), 4)

        by_name = {value["name"]: value for value in benchmarks}
        token_source = "oxd_preda/simulator/contracts/Token.prd"
        self.assertEqual(by_name["TokenParallel"]["source"], token_source)
        self.assertEqual(by_name["AirDropParallel"]["source"], token_source)

        for benchmark in benchmarks:
            self.assertIn(benchmark["source"], EXPECTED_ORIGINAL_SOURCES)
            self.assertTrue(benchmark.get("certificates"))
            for selector in benchmark["sites"].values():
                self.assertNotIn("id", selector)
                self.assertNotIn("relay_site_id", selector)
            for expectation in benchmark["certificates"]:
                self.assertTrue(expectation.get("mutation"))

        required_mutations = {
            "CoEmissionIndependent": "IntroduceAlias",
            "MutualExclusive": "GuardNegate",
            "MustPrecede": "RelayOrderSwap",
        }
        observed = {
            certificate["certificate_type"]: certificate["mutation"]["kind"]
            for benchmark in benchmarks
            for certificate in benchmark["certificates"]
            if certificate["certificate_type"] in required_mutations
        }
        self.assertEqual(observed, required_mutations)
        self.assertTrue(
            {"GuardNegate", "IntroduceAlias"}.issubset(
                set(mutation_kinds(by_name["BallotParallel"]))
                | set(mutation_kinds(by_name["TokenParallel"]))
            )
        )

        projected = load_configuration(config_path)["workloads"]
        self.assertEqual(len(projected), 5)
        for workload in projected:
            self.assertEqual(workload["baseline_source"], workload["source_file"])
            self.assertIn(workload["source_file"], EXPECTED_ORIGINAL_SOURCES)
            self.assertEqual(
                workload["source"],
                workload["source_file"] + "#" + workload["function_signature"],
            )
            for expectation in workload["expected_certificates"]:
                if expectation["certificate_type"] in {
                    "CoEmissionIndependent",
                    "MutualExclusive",
                    "MustPrecede",
                }:
                    self.assertIsInstance(expectation["site_a"], dict)
                    self.assertIsInstance(expectation["site_b"], dict)


if __name__ == "__main__":
    unittest.main()
