#!/usr/bin/env python3
"""Run R-PREDA real-program coverage and synthetic scalability studies."""

from __future__ import annotations

import argparse
import collections
import csv
import datetime as dt
import hashlib
import json
import math
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Set, Tuple


HERE = pathlib.Path(__file__).resolve().parent
MUTATION_TOOL = HERE.parent / "rpreda_mutation"
if str(MUTATION_TOOL) not in sys.path:
    sys.path.insert(0, str(MUTATION_TOOL))

from CoverageAnalyzer import (  # noqa: E402
    REQUESTED_PROPERTIES,
    REQUESTED_STATUSES,
    UNKNOWN_REASON_CATEGORIES,
    ManifestValidationError,
    analyze_manifest,
    load_json,
    validate_manifest,
)
from FigureEmitter import certificate_distribution, scalability_curve, unknown_reasons  # noqa: E402
from SyntheticPredaGenerator import GeneratorConfigurationError, generate_program  # noqa: E402

try:
    from MutationRunner import compile_source, read_json, run_runtime_strict
except ImportError as exc:  # pragma: no cover - configuration diagnostic
    raise SystemExit("cannot import the existing baseline compile/trace harness: %s" % exc)


DEFAULT_CONFIG = HERE / "benchmarks.json"

ANALYSIS_PHASES = {
    "cfg_construction",
    "call_graph_construction",
    "effect_analysis",
    "icfg_construction",
    "summary_analysis",
    "refinement_generation",
    "refinement_solver",
    "refinement_total",
    "certificate_generation",
    "analysis_total",
    "manifest_emission",
}

REQUIRED_SCALABILITY_BUILD_FEATURES = {
    "analysis_profiling": True,
    "runtime_trace": False,
    "runtime_optimization": False,
    "z3": True,
    "bound_manifest": True,
}

COUPLED_SWEEP_DIMENSIONS = {
    "relay_sites": ["relay_width"],
    # Holding relay-site count fixed means width necessarily changes depth.  A
    # common function-count floor is also imposed across this sweep so the
    # narrowest graph does not silently receive fewer functions than the rest.
    "relay_width": ["relay_depth", "functions"],
    "relay_depth": ["relay_width"],
}

SOLVER_CALLED_STATUSES = frozenset(
    ("Proved", "Disproved", "Unknown", "InconsistentAssumptions", "EncodingError")
)


class EvaluationError(RuntimeError):
    pass


def _repo_root() -> pathlib.Path:
    configured = os.environ.get("RPREDA_REPO_ROOT")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    for anchor in (HERE, pathlib.Path.cwd().resolve()):
        for candidate in (anchor, *anchor.parents):
            if (candidate / "oxd_preda").is_dir() and (candidate / "bin").is_dir():
                return candidate
    raise EvaluationError("cannot locate PREDA repository; set RPREDA_REPO_ROOT")


REPO_ROOT = _repo_root()


def _utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_sha256(value: Any) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def _portable(path: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return "external/" + path.name


def _resolve(value: str, config_directory: pathlib.Path) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if path.is_absolute():
        return path.resolve()
    for candidate in (REPO_ROOT / path, config_directory / path):
        if candidate.exists():
            return candidate.resolve()
    return (REPO_ROOT / path).resolve()


def _load_config(path: pathlib.Path) -> Mapping[str, Any]:
    value = load_json(path)
    if int(value.get("schema_version", 0)) != 1:
        raise EvaluationError("evaluation config schema_version must be 1")
    benchmarks = value.get("real_world_benchmarks")
    if not isinstance(benchmarks, list) or not benchmarks:
        raise EvaluationError("evaluation config has no real_world_benchmarks")
    return value


def _percentage(numerator: int, denominator: int) -> Optional[float]:
    return None if denominator == 0 else round(100.0 * numerator / denominator, 3)


def _sum_coverage(items: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    eligible = sum(int(item.get("eligible", 0)) for item in items)
    covered = sum(int(item.get("covered", 0)) for item in items)
    return {"eligible": eligible, "covered": covered, "percentage": _percentage(covered, eligible)}


def _aggregate_runtime(rows: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    values = [row.get("runtime_weighted_coverage", {}) for row in rows if row.get("runtime_weighted_coverage", {}).get("available")]
    observed = sum(int(value.get("relay_emissions", {}).get("observed", 0)) for value in values)
    identity = sum(int(value.get("relay_emissions", {}).get("manifest_identity_covered", 0)) for value in values)
    emissions = sum(int(value.get("relay_emissions", {}).get("certificate_covered", 0)) for value in values)
    pair_instances = sum(int(value.get("relay_pairs", {}).get("observed", 0)) for value in values)
    distinct_pairs = sum(int(value.get("relay_pairs", {}).get("observed_distinct_site", 0)) for value in values)
    pairs = sum(int(value.get("relay_pairs", {}).get("certified", 0)) for value in values)
    trees = sum(int(value.get("relay_trees", {}).get("executed", 0)) for value in values)
    work = sum(int(value.get("relay_trees", {}).get("finite_work_bound", 0)) for value in values)
    depth = sum(int(value.get("relay_trees", {}).get("finite_depth_bound", 0)) for value in values)
    transactions = sum(int(value.get("transactions", {}).get("source_transactions", 0)) for value in values)
    complete = sum(int(value.get("transactions", {}).get("complete", 0)) for value in values)
    partial = sum(int(value.get("transactions", {}).get("partial", 0)) for value in values)
    fallback = sum(int(value.get("transactions", {}).get("fallback", 0)) for value in values)
    return {
        "available": bool(values),
        "reason": "" if values else "no benchmark has an available runtime trace",
        "benchmark_count": len(values),
        "relay_emissions": {
            "observed": observed,
            "manifest_identity_covered": identity,
            "manifest_identity_percentage": _percentage(identity, observed),
            "certificate_covered": emissions,
            "percentage": _percentage(emissions, observed),
        },
        "relay_pairs": {
            "observed": pair_instances,
            "observed_distinct_site": distinct_pairs,
            "certified": pairs,
            "percentage": _percentage(pairs, distinct_pairs),
            "all_pair_instance_percentage": _percentage(pairs, pair_instances),
        },
        "relay_trees": {
            "executed": trees,
            "finite_work_bound": work,
            "finite_depth_bound": depth,
            "work_percentage": _percentage(work, trees),
            "depth_percentage": _percentage(depth, trees),
        },
        "transactions": {
            "source_transactions": transactions,
            "complete": complete,
            "partial": partial,
            "fallback": fallback,
            "complete_percentage": _percentage(complete, transactions),
            "partial_percentage": _percentage(partial, transactions),
            "fallback_percentage": _percentage(fallback, transactions),
        },
    }


def _aggregate_static(rows: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    program_keys = (
        "functions",
        "source_functions",
        "generated_relay_lambda_functions",
        "cfg_nodes_excluding_entry_exit",
        "relay_sites",
        "resolved_synchronous_calls",
        "unresolved_calls",
        "cfg_complete_functions",
        "cfg_analyzable_functions",
        "icfg_complete_functions",
        "icfg_analyzable_functions",
    )
    program = {key: sum(int(row["program_coverage"].get(key, 0)) for row in rows) for key in program_keys}
    program["contracts"] = len(rows)
    program["cfg_completeness_percentage"] = _percentage(program["cfg_complete_functions"], program["functions"])
    program["cfg_analyzable_percentage"] = _percentage(program["cfg_analyzable_functions"], program["functions"])
    program["icfg_completeness_percentage"] = _percentage(program["icfg_complete_functions"], program["functions"])
    program["icfg_analyzable_percentage"] = _percentage(program["icfg_analyzable_functions"], program["functions"])
    protocol_fields = (
        "relay_sites_covered",
        "handler_relations",
        "direct_logical_work_bound",
        "transitive_logical_work_bound",
        "physical_route_bound",
        "relay_tree_depth_bound",
        "fanout_information",
    )
    protocol: Dict[str, Any] = {
        "functions_with_relay_protocol": sum(int(row["protocol_coverage"].get("functions_with_relay_protocol", 0)) for row in rows)
    }
    for field in protocol_fields:
        protocol[field] = _sum_coverage([row["protocol_coverage"].get(field, {}) for row in rows])
    refinement_fields = (
        "target_formulas",
        "target_formulas_custom_scope",
        "argument_formulas",
        "guard_formulas",
        "nontrivial_guard_formulas",
        "guard_equivalence",
        "count_constraints",
        "count_upper_constraints",
        "count_nonnegative_constraints",
        "depth_constraints",
    )
    refinement = {field: _sum_coverage([row["refinement_coverage"].get(field, {}) for row in rows]) for field in refinement_fields}
    refinement["depth_constraints"].update(
        {
            "available_in_refinement_ir": False,
            "represented_elsewhere": "parallel_certificate.functions[].relay_tree_depth",
        }
    )
    return {
        "unique_artifacts": len(rows),
        "dedup_key": "artifact_binding.module_id",
        "program_coverage": program,
        "protocol_coverage": protocol,
        "refinement_coverage": refinement,
    }


def _certificate_payload(rows: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    records: List[Mapping[str, Any]] = []
    by_benchmark: Dict[str, Any] = {}
    for row in rows:
        quality = row["certificate_quality"]
        by_benchmark[str(row["benchmark"])] = {
            "counts": quality["counts"],
            "detailed_outcomes": quality["detailed_outcomes"],
            "unknown_reason_breakdown": quality["unknown_reason_breakdown"],
        }
        for record in quality["records"]:
            records.append({"benchmark": row["benchmark"], **record})
    summary = {prop: {status: 0 for status in REQUESTED_STATUSES} for prop in REQUESTED_PROPERTIES}
    detailed: Dict[str, collections.Counter] = {prop: collections.Counter() for prop in REQUESTED_PROPERTIES}
    reasons: collections.Counter = collections.Counter()
    reason_multilabel: collections.Counter = collections.Counter()
    for record in records:
        prop, outcome = str(record["property"]), str(record["status"])
        detailed[prop][outcome] += 1
        projected = "Unknown" if outcome in ("NotEstablished", "InvalidEvidence") else outcome
        if projected in REQUESTED_STATUSES:
            summary[prop][projected] += 1
            if projected in ("Unknown", "Unsupported"):
                reasons[str(record.get("unknown_reason") or "other_conservative")] += 1
                for category in record.get("reason_categories", []) or ["other_conservative"]:
                    reason_multilabel[str(category)] += 1
    return {
        "schema_version": 1,
        "status_projection": {
            "paper_columns": list(REQUESTED_STATUSES),
            "NotEstablished": "Unknown",
            "InvalidEvidence": "Unknown",
            "Disproved": "reported separately",
            "NotApplicable": "reported separately",
        },
        "summary": {
            "by_property": summary,
            "detailed_outcomes": {key: dict(sorted(value.items())) for key, value in detailed.items()},
            "unknown_reason_breakdown": {
                category: int(reasons.get(category, 0))
                for category in UNKNOWN_REASON_CATEGORIES
            },
            "unknown_reason_multilabel": {
                category: int(reason_multilabel.get(category, 0))
                for category in UNKNOWN_REASON_CATEGORIES
            },
            "candidate_denominators": {
                key: sum(int(count) for count in value.values())
                for key, value in detailed.items()
            },
        },
        "by_benchmark": by_benchmark,
        "records": records,
        "diagnostics": [],
    }


def _coverage_csv(path: pathlib.Path, rows: Sequence[Mapping[str, Any]], aggregate: Mapping[str, Any]) -> None:
    coverage_columns = (
        "fully_linked_sites", "handler_relations", "direct_bound", "transitive_bound",
        "physical_bound", "depth_bound", "fanout_information", "target_formulas",
        "target_formulas_custom_scope", "argument_formulas", "guard_formulas",
        "nontrivial_guard_formulas", "guard_equivalence", "count_constraints",
        "count_upper_constraints", "count_nonnegative_constraints", "depth_constraints",
    )
    fields = [
        "record_type", "benchmark", "contract", "analysis_scope", "included_in_static_aggregate",
        "module_id", "functions", "source_functions", "generated_relay_lambdas", "relay_sites",
        "resolved_sync_calls", "unresolved_calls", "cfg_complete", "cfg_analyzable",
        "icfg_complete", "icfg_analyzable", "protocol_functions",
        *("%s_%s" % (name, suffix) for name in coverage_columns for suffix in ("eligible", "covered", "percentage")),
        "runtime_available", "runtime_reason", "runtime_source_transactions", "runtime_emissions",
        "runtime_emissions_covered", "runtime_emission_percentage", "runtime_pair_instances",
        "runtime_distinct_pairs", "runtime_pairs_covered", "runtime_pair_percentage",
        "runtime_all_pair_instance_percentage", "runtime_distinct_pair_percentage",
        "runtime_trees", "runtime_trees_finite_work", "runtime_tree_work_percentage",
        "runtime_trees_finite_depth", "runtime_tree_depth_percentage", "runtime_complete",
        "runtime_partial", "runtime_fallback",
    ]

    def populate(
        benchmark: str,
        contract: str,
        scope: str,
        module_id: str,
        included: Any,
        program: Mapping[str, Any],
        protocol: Mapping[str, Any],
        refinement: Mapping[str, Any],
        runtime: Mapping[str, Any],
    ) -> Mapping[str, Any]:
        metrics = {
            "fully_linked_sites": protocol.get("relay_sites_covered", {}),
            "handler_relations": protocol.get("handler_relations", {}),
            "direct_bound": protocol.get("direct_logical_work_bound", {}),
            "transitive_bound": protocol.get("transitive_logical_work_bound", {}),
            "physical_bound": protocol.get("physical_route_bound", {}),
            "depth_bound": protocol.get("relay_tree_depth_bound", {}),
            "fanout_information": protocol.get("fanout_information", {}),
            "target_formulas": refinement.get("target_formulas", {}),
            "target_formulas_custom_scope": refinement.get("target_formulas_custom_scope", {}),
            "argument_formulas": refinement.get("argument_formulas", {}),
            "guard_formulas": refinement.get("guard_formulas", {}),
            "nontrivial_guard_formulas": refinement.get("nontrivial_guard_formulas", {}),
            "guard_equivalence": refinement.get("guard_equivalence", {}),
            "count_constraints": refinement.get("count_constraints", {}),
            "count_upper_constraints": refinement.get("count_upper_constraints", {}),
            "count_nonnegative_constraints": refinement.get("count_nonnegative_constraints", {}),
            "depth_constraints": refinement.get("depth_constraints", {}),
        }
        value: Dict[str, Any] = {
            "record_type": "aggregate" if scope == "unique_static_aggregate" else "benchmark",
            "benchmark": benchmark, "contract": contract, "analysis_scope": scope,
            "included_in_static_aggregate": included, "module_id": module_id,
            "functions": program.get("functions", 0), "source_functions": program.get("source_functions", 0),
            "generated_relay_lambdas": program.get("generated_relay_lambda_functions", 0),
            "relay_sites": program.get("relay_sites", 0), "resolved_sync_calls": program.get("resolved_synchronous_calls", 0),
            "unresolved_calls": program.get("unresolved_calls", 0), "cfg_complete": program.get("cfg_complete_functions", 0),
            "cfg_analyzable": program.get("cfg_analyzable_functions", 0), "icfg_complete": program.get("icfg_complete_functions", 0),
            "icfg_analyzable": program.get("icfg_analyzable_functions", 0),
            "protocol_functions": protocol.get("functions_with_relay_protocol", 0),
            "runtime_available": bool(runtime.get("available", False)),
            "runtime_reason": runtime.get("reason", ""),
            "runtime_source_transactions": runtime.get("transactions", {}).get("source_transactions", 0),
            "runtime_emissions": runtime.get("relay_emissions", {}).get("observed", 0),
            "runtime_emissions_covered": runtime.get("relay_emissions", {}).get("certificate_covered", 0),
            "runtime_emission_percentage": runtime.get("relay_emissions", {}).get("percentage"),
            "runtime_pair_instances": runtime.get("relay_pairs", {}).get("observed", 0),
            "runtime_distinct_pairs": runtime.get("relay_pairs", {}).get("observed_distinct_site", 0),
            "runtime_pairs_covered": runtime.get("relay_pairs", {}).get("certified", 0),
            "runtime_pair_percentage": runtime.get("relay_pairs", {}).get("percentage"),
            "runtime_all_pair_instance_percentage": runtime.get("relay_pairs", {}).get("all_pair_instance_percentage"),
            "runtime_distinct_pair_percentage": runtime.get("relay_pairs", {}).get("percentage"),
            "runtime_trees": runtime.get("relay_trees", {}).get("executed", 0),
            "runtime_trees_finite_work": runtime.get("relay_trees", {}).get("finite_work_bound", 0),
            "runtime_tree_work_percentage": runtime.get("relay_trees", {}).get("work_percentage"),
            "runtime_trees_finite_depth": runtime.get("relay_trees", {}).get("finite_depth_bound", 0),
            "runtime_tree_depth_percentage": runtime.get("relay_trees", {}).get("depth_percentage"),
            "runtime_complete": runtime.get("transactions", {}).get("complete", 0),
            "runtime_partial": runtime.get("transactions", {}).get("partial", 0),
            "runtime_fallback": runtime.get("transactions", {}).get("fallback", 0),
        }
        for name, metric in metrics.items():
            for suffix in ("eligible", "covered", "percentage"):
                value["%s_%s" % (name, suffix)] = metric.get(suffix)
        return value

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            p, q, f, runtime = row["program_coverage"], row["protocol_coverage"], row["refinement_coverage"], row["runtime_weighted_coverage"]
            writer.writerow(populate(row["benchmark"], row["contract"], row["analysis_scope"], row["artifact"]["module_id"], row["include_in_static_aggregate"], p, q, f, runtime))
        writer.writerow(populate("__static_aggregate__", "", "unique_static_aggregate", "", True, aggregate["program_coverage"], aggregate["protocol_coverage"], aggregate["refinement_coverage"], {}))
        writer.writerow(populate("__runtime_aggregate__", "", "all_workload_runtime_aggregate", "", "N/A", {}, {}, {}, aggregate["runtime_weighted_coverage"]))


def _certificate_csv(path: pathlib.Path, payload: Mapping[str, Any]) -> None:
    fields = ["record_type", "benchmark", "property", "subproperty", "status", "count", "candidate_count", "source_function_id", "site_a", "site_b", "certificate_id", "relation", "proof_strength", "evidence_valid", "cfg_evidence_ids", "constraint_evidence_ids", "solver_evidence_ids", "reason", "reason_category", "reason_categories", "evidence_validation_reason"]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for prop, statuses in payload["summary"]["by_property"].items():
            for status, count in statuses.items():
                writer.writerow({"record_type": "property_summary", "property": prop, "status": status, "count": count, "candidate_count": payload["summary"]["candidate_denominators"].get(prop, 0)})
        for prop, statuses in payload["summary"]["detailed_outcomes"].items():
            for status, count in statuses.items():
                writer.writerow({"record_type": "detailed_outcome_summary", "property": prop, "status": status, "count": count})
        for reason, count in payload["summary"]["unknown_reason_breakdown"].items():
            writer.writerow({"record_type": "reason_summary", "count": count, "reason_category": reason})
        for record in payload["records"]:
            evidence = record.get("evidence", {})
            writer.writerow({
                "record_type": "property_record", "benchmark": record.get("benchmark", ""), "property": record.get("property", ""), "subproperty": record.get("subproperty", ""), "status": record.get("status", ""), "count": 1,
                "source_function_id": record.get("benchmark_function_id", ""), "site_a": record.get("site_a", ""), "site_b": record.get("site_b", ""), "certificate_id": record.get("certificate_id", ""), "relation": record.get("relation", ""), "proof_strength": record.get("proof_strength", ""), "evidence_valid": record.get("evidence_valid", ""),
                "cfg_evidence_ids": json.dumps(evidence.get("cfg_fact_ids", []), separators=(",", ":")), "constraint_evidence_ids": json.dumps(evidence.get("constraint_ids", []), separators=(",", ":")), "solver_evidence_ids": json.dumps(evidence.get("solver_result_ids", []), separators=(",", ":")), "reason": record.get("reason", ""), "reason_category": record.get("unknown_reason", ""),
                "reason_categories": json.dumps(record.get("reason_categories", []), separators=(",", ":")), "evidence_validation_reason": record.get("evidence_validation_reason", ""),
            })


def _runtime_arguments(defaults: Mapping[str, Any], benchmark: Mapping[str, Any]) -> List[str]:
    result = ["-seed:%d" % int(defaults.get("seed", 88)), "-order:%d" % int(defaults.get("order", 2))]
    parameters = benchmark.get("runtime_parameters", {})
    if isinstance(parameters, Mapping):
        result.extend("-%s:%s" % (key, parameters[key]) for key in sorted(parameters))
    result.extend(str(value) for value in benchmark.get("runtime_args", []))
    return result


def run_real_world(
    config: Mapping[str, Any],
    config_path: pathlib.Path,
    output_root: pathlib.Path,
    work_root: pathlib.Path,
    chsimu: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
    reuse_artifacts: Optional[pathlib.Path],
) -> Tuple[Mapping[str, Any], Mapping[str, Any]]:
    defaults = config.get("defaults", {}) if isinstance(config.get("defaults"), Mapping) else {}
    timeout = float(defaults.get("timeout_seconds", 60))
    rows: List[Mapping[str, Any]] = []
    diagnostics: List[Mapping[str, Any]] = []
    for benchmark in config["real_world_benchmarks"]:
        name = str(benchmark["id"])
        source = _resolve(str(benchmark["source"]), config_path.parent)
        manifest: Optional[Mapping[str, Any]] = None
        trace: Optional[Mapping[str, Any]] = None
        compile_status = "NotRun"
        runtime_status = "NotRun"
        if reuse_artifacts is not None:
            artifact_name = str(benchmark.get("artifact_key", name))
            base = reuse_artifacts / "benchmarks" / artifact_name / "baseline"
            manifest_path = base / "oracle.relay_protocol.json"
            trace_path = base / "runtime" / "trace.json"
            if manifest_path.is_file():
                manifest = load_json(manifest_path)
                compile_status = "Reused"
            if trace_path.is_file() and benchmark.get("runtime_enabled", True):
                trace = load_json(trace_path)
                runtime_status = "Reused"
        if manifest is None:
            compile_result = compile_source(source, work_root / name / "compile", chsimu, library_paths, path_prefixes, timeout, work_root)
            compile_status = compile_result.status
            manifest = compile_result.manifest
        if manifest is None:
            diagnostics.append({"benchmark": name, "stage": "compile", "status": compile_status, "reason": "no manifest"})
            continue
        validate_manifest(manifest)
        if trace is None and benchmark.get("runtime_enabled", True):
            template = _resolve(str(benchmark["runtime_template"]), config_path.parent)
            runtime = run_runtime_strict(
                source, template, work_root / name / "runtime", chsimu, library_paths, path_prefixes,
                timeout, _runtime_arguments(defaults, benchmark), benchmark.get("runtime_coverage"), work_root,
            )
            runtime_status = str(runtime.get("status", "Unknown"))
            trace_path = work_root / name / "runtime" / "trace.json"
            if trace_path.is_file():
                trace = load_json(trace_path)
            if runtime_status != "Passed":
                diagnostics.append({"benchmark": name, "stage": "runtime", "status": runtime_status, "reason": runtime.get("reason", "")})
        static_filters = [str(value) for value in benchmark.get("static_function_filters", [])]
        runtime_filters = [str(value) for value in benchmark.get("runtime_function_filters", static_filters)]
        static_row = dict(analyze_manifest(name, manifest, None, static_filters, _portable(source)))
        runtime_row = analyze_manifest(name, manifest, trace, runtime_filters, _portable(source))
        static_row["runtime_weighted_coverage"] = runtime_row["runtime_weighted_coverage"]
        binding = manifest.get("artifact_binding", {})
        static_row["artifact"] = {
            "module_id": str(binding.get("module_id", "")), "manifest_hash": str(binding.get("manifest_hash", "")),
            "source_sha256": _sha256(source), "binding_complete": bool(binding.get("binding_complete")),
        }
        static_row["include_in_static_aggregate"] = bool(benchmark.get("include_in_static_aggregate", True))
        static_row["aggregate_exclusion_reason"] = str(benchmark.get("aggregate_exclusion_reason", ""))
        static_row["execution_status"] = {"compile": compile_status, "runtime": runtime_status}
        rows.append(static_row)

    # Deduplicate static artifacts by compiler-bound module identity. A config
    # exclusion (AirDrop workload slice) is applied before deduplication.
    unique: Dict[str, Mapping[str, Any]] = {}
    for row in rows:
        if not row["include_in_static_aggregate"]:
            continue
        module_id = str(row["artifact"]["module_id"])
        if not module_id:
            raise EvaluationError("%s has no bound module_id" % row["benchmark"])
        unique.setdefault(module_id, row)
    static_rows = list(unique.values())
    aggregate = _aggregate_static(static_rows)
    aggregate["runtime_weighted_coverage"] = _aggregate_runtime(rows)
    coverage_payload = {
        "schema_version": 1,
        "metadata": {
            "generated_at": _utc_now(), "config": _portable(config_path), "config_sha256": _sha256(config_path),
            "manifest_schema_required": 5, "static_dedup_key": "artifact_binding.module_id",
            "runtime_pair_denominators": ["all_pair_instances", "distinct_site_pair_instances"],
            "execution_mode": "Reused" if reuse_artifacts is not None else "Fresh",
            "chsimu": _portable(chsimu),
            "chsimu_sha256": _sha256(chsimu) if chsimu.is_file() else "",
            "host": {"system": platform.system(), "machine": platform.machine(), "logical_cpus": os.cpu_count()},
        },
        "benchmarks": rows,
        "aggregate": aggregate,
        "diagnostics": diagnostics,
    }
    certificate_payload = _certificate_payload(static_rows)
    certificate_payload["metadata"] = coverage_payload["metadata"]
    coverage_dir, certificate_dir = output_root / "coverage", output_root / "certificate"
    _write_json(coverage_dir / "coverage.json", coverage_payload)
    _coverage_csv(coverage_dir / "coverage.csv", rows, aggregate)
    _write_json(certificate_dir / "certificate_quality.json", certificate_payload)
    _certificate_csv(certificate_dir / "certificate_quality.csv", certificate_payload)
    certificate_distribution(
        certificate_payload["summary"]["by_property"],
        certificate_dir / "certificate_distribution.svg",
        certificate_payload["summary"]["candidate_denominators"],
    )
    unknown_reasons(certificate_payload["summary"]["unknown_reason_breakdown"], certificate_dir / "unknown_reason_breakdown.svg")
    return coverage_payload, certificate_payload


def _case_configurations(config: Mapping[str, Any]) -> List[Tuple[str, str, Mapping[str, Any]]]:
    scalability = config.get("scalability", {})
    if not isinstance(scalability, Mapping) or not isinstance(scalability.get("base"), Mapping):
        raise EvaluationError("scalability.base is missing")
    base = dict(scalability["base"])
    cases: List[Tuple[str, str, Mapping[str, Any]]] = []
    sweeps = scalability.get("sweeps", {})
    if not isinstance(sweeps, Mapping):
        raise EvaluationError("scalability.sweeps must be an object")
    width_values = sweeps.get("relay_width", [])
    if not isinstance(width_values, list):
        raise EvaluationError("scalability sweep relay_width must be an array")
    fixed_width_sites = int(scalability.get("relay_width_fixed_sites", 16))
    width_function_floor = int(base.get("functions", 0))
    for raw_width in width_values:
        width = int(raw_width)
        if width <= 0:
            raise EvaluationError("relay_width sweep values must be positive")
        structural_minimum = (
            int(math.ceil(float(fixed_width_sites) / width))
            + int(base.get("sync_call_depth", 0))
            + 1
        )
        width_function_floor = max(width_function_floor, structural_minimum)
    for axis, values in sweeps.items():
        if not isinstance(values, list):
            raise EvaluationError("scalability sweep %s must be an array" % axis)
        for value in values:
            case = dict(base)
            case[str(axis)] = int(value)
            if axis == "relay_sites":
                case["relay_depth"] = 1 if int(value) else 0
                case["relay_width"] = max(1, int(value))
            elif axis == "relay_width":
                fixed_sites = fixed_width_sites
                if int(value) <= 0:
                    raise EvaluationError("relay_width sweep values must be positive")
                case["relay_sites"] = fixed_sites
                case["relay_width"] = int(value)
                case["relay_depth"] = int(math.ceil(float(fixed_sites) / int(value)))
                # Use one common function count for every width point.  The
                # maximum structural minimum is dictated by the narrowest
                # graph (17 for the checked-in 16-site sweep).
                case["functions"] = width_function_floor
            elif axis == "relay_depth":
                fixed_sites = int(scalability.get("relay_depth_fixed_sites", 8))
                if int(value) <= 0:
                    raise EvaluationError("relay_depth sweep values must be positive")
                case["relay_sites"] = fixed_sites
                case["relay_width"] = int(math.ceil(float(fixed_sites) / int(value)))
                case["functions"] = max(int(case["functions"]), int(value) + 1)
            case_id = "%s_%s" % (axis, value)
            cases.append((case_id, str(axis), case))
    return cases


def _derive_round_seed(base_seed: int, round_kind: str, round_index: int) -> int:
    """Derive a stable per-round seed without depending on Python hash state."""
    material = "rpreda-scalability-round-v1|%d|%s|%d" % (
        int(base_seed), round_kind, int(round_index)
    )
    # Thirteen hexadecimal digits stay below 2^53, so the JSON number remains
    # exact in both Python and common JavaScript tooling.
    return int(hashlib.sha256(material.encode("utf-8")).hexdigest()[:13], 16)


def _deterministic_case_order(case_ids: Sequence[str], round_seed: int) -> List[str]:
    """Return a deterministic hash-shuffled permutation for one round."""
    return sorted(
        (str(case_id) for case_id in case_ids),
        key=lambda case_id: (
            hashlib.sha256(("%d|%s" % (round_seed, case_id)).encode("utf-8")).hexdigest(),
            case_id,
        ),
    )


def _build_round_schedule(
    case_ids: Sequence[str], warmup_count: int, measured_count: int, base_seed: int
) -> List[Mapping[str, Any]]:
    """Build all warmup rounds followed by all measured rounds."""
    schedule: List[Mapping[str, Any]] = []
    global_round = 0
    for round_kind, count in (("warmup", warmup_count), ("measured", measured_count)):
        for round_index in range(count):
            round_seed = _derive_round_seed(base_seed, round_kind, round_index)
            schedule.append(
                {
                    "round": global_round,
                    "round_kind": round_kind,
                    "round_index": round_index,
                    "round_seed": round_seed,
                    "case_order": _deterministic_case_order(case_ids, round_seed),
                }
            )
            global_round += 1
    return schedule


def _formula_nodes(value: Any) -> int:
    if isinstance(value, Mapping):
        return int("kind" in value and "sort" in value) + sum(_formula_nodes(child) for child in value.values())
    if isinstance(value, list):
        return sum(_formula_nodes(child) for child in value)
    return 0


def _normalized_manifest(manifest: Mapping[str, Any]) -> Mapping[str, Any]:
    value = json.loads(json.dumps(manifest))
    def normalize_timings(node: Any) -> None:
        if isinstance(node, MutableMapping):
            if "elapsed_time_ms" in node:
                node["elapsed_time_ms"] = 0
            if "elapsed_time_us" in node:
                node["elapsed_time_us"] = 0
            for child in node.values():
                normalize_timings(child)
        elif isinstance(node, list):
            for child in node:
                normalize_timings(child)
    normalize_timings(value)
    for function in value.get("parallel_certificate", {}).get("functions", []):
        if isinstance(function, MutableMapping):
            function.pop("direct_work_bound", None)
            function.pop("transitive_work_bound", None)
            function.pop("depth_bound", None)
    return value


def _formula_ir_projection(refinement: Mapping[str, Any]) -> Mapping[str, Any]:
    obligations = []
    for raw in refinement.get("proof_obligations", []):
        if not isinstance(raw, Mapping):
            continue
        value = dict(raw)
        value.pop("solver_result", None)
        obligations.append(value)
    return {
        "symbols": refinement.get("symbols", []),
        "constraints": refinement.get("constraints", []),
        "proof_obligations": obligations,
    }


def _longest_graph_depth(manifest: Mapping[str, Any], edge_kind: str, root_fragment: str) -> int:
    if edge_kind == "relay":
        handlers = {str(value.get("id", "")): str(value.get("target_function_id", "")) for value in manifest.get("handlers", []) if isinstance(value, Mapping)}
        graph: MutableMapping[str, Set[str]] = collections.defaultdict(set)
        for edge in manifest.get("edges", []):
            if isinstance(edge, Mapping) and edge.get("resolved"):
                graph[str(edge.get("source_function_id", ""))].add(handlers.get(str(edge.get("handler_id", "")), ""))
    else:
        graph = collections.defaultdict(set)
        for edge in manifest.get("control_flow", {}).get("synchronous_call_graph", {}).get("edges", []):
            if isinstance(edge, Mapping) and edge.get("kind") == "Synchronous" and edge.get("resolved"):
                graph[str(edge.get("caller", ""))].add(str(edge.get("callee", "")))
    roots = [value for value in graph if root_fragment in value]
    if not roots:
        return 0

    def visit(node: str, active: Set[str]) -> int:
        if node in active:
            return 0
        children = [child for child in graph.get(node, set()) if child]
        if not children:
            return 0
        return 1 + max(visit(child, active | {node}) for child in children)

    return max(visit(root, set()) for root in roots)


def _validate_synthetic(manifest: Mapping[str, Any], metadata: Mapping[str, Any]) -> Mapping[str, Any]:
    requested, derived = metadata["requested"], metadata["derived"]
    sites = [value for value in manifest.get("relay_sites", []) if isinstance(value, Mapping)]
    widths = collections.Counter(str(value.get("source_function_id", "")) for value in sites)

    def additive_terms(expression: Any) -> int:
        if not isinstance(expression, Mapping):
            return 0
        children = expression.get("children", [])
        if expression.get("kind") == "group" and isinstance(children, list) and len(children) == 1:
            return additive_terms(children[0])
        if expression.get("kind") == "binary" and expression.get("operator") == "+" and isinstance(children, list) and len(children) == 2:
            return additive_terms(children[0]) + additive_terms(children[1])
        return 1

    argument_counts = {len(value.get("arguments", [])) for value in sites}
    target_term_counts = {additive_terms(value.get("target")) for value in sites}
    actual = {
        "functions": len(manifest.get("control_flow", {}).get("functions", [])),
        "relay_sites": len(sites),
        "relay_width": max(widths.values()) if widths else 0,
        "relay_depth": _longest_graph_depth(manifest, "relay", "::root("),
        "branch_depth": max([len(value.get("branches", [])) for value in sites] + [0]),
        "loop_depth": max([len(value.get("loops", [])) for value in sites] + [0]),
        "sync_call_depth": _longest_graph_depth(manifest, "sync", "::root("),
        "arguments_per_relay": next(iter(argument_counts)) if len(argument_counts) == 1 else -1,
        "target_expression_terms": next(iter(target_term_counts)) if len(target_term_counts) == 1 else -1,
    }
    expected = {key: int(derived.get(key, requested.get(key, 0))) for key in actual}
    mismatches = {key: {"expected": expected[key], "actual": actual[key]} for key in actual if expected[key] != actual[key]}
    return {"valid": not mismatches, "expected": expected, "actual": actual, "mismatches": mismatches}


def _percentile(values: Sequence[float], percentile: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * percentile
    lower, upper = int(math.floor(position)), int(math.ceil(position))
    if lower == upper:
        return float(ordered[lower])
    return float(ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower))


def _distribution(values: Sequence[float], digits: int = 6) -> Mapping[str, Any]:
    numeric = [float(value) for value in values]
    if not numeric:
        return {
            "n": 0,
            "min": None,
            "q1": None,
            "median": None,
            "q3": None,
            "max": None,
            "iqr": None,
            "p95": None,
            "p95_is_descriptive": True,
            "tail_inference_supported": False,
        }
    q1 = _percentile(numeric, 0.25)
    q3 = _percentile(numeric, 0.75)
    return {
        "n": len(numeric),
        "min": round(min(numeric), digits),
        "q1": round(q1, digits),
        "median": round(statistics.median(numeric), digits),
        "q3": round(q3, digits),
        "max": round(max(numeric), digits),
        "iqr": round(q3 - q1, digits),
        "p95": round(_percentile(numeric, 0.95), digits),
        "p95_is_descriptive": True,
        "tail_inference_supported": False,
    }


def _timing_stability_control(samples: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    """Summarize repeated, byte-identical baseline manifests across rounds."""
    baseline = [sample for sample in samples if bool(sample.get("baseline_configuration"))]
    groups: MutableMapping[str, List[Mapping[str, Any]]] = collections.defaultdict(list)
    for sample in baseline:
        digest = str(sample.get("normalized_manifest_sha256", ""))
        if digest:
            groups[digest].append(sample)
    candidates = [
        (digest, values)
        for digest, values in groups.items()
        if len({str(value.get("case_id", "")) for value in values}) >= 2
    ]
    if not candidates:
        return {
            "available": False,
            "reason": "fewer than two baseline case IDs produced an identical normalized manifest",
            "round_median_max_over_min": None,
            "case_median_max_over_min": None,
            "coefficient_of_variation": None,
        }
    digest, selected = sorted(
        candidates,
        key=lambda item: (
            -len({str(value.get("case_id", "")) for value in item[1]}),
            -len(item[1]),
            item[0],
        ),
    )[0]
    timings = [float(value.get("total_analysis_ms", 0.0)) for value in selected]
    by_round: MutableMapping[int, List[float]] = collections.defaultdict(list)
    by_case: MutableMapping[str, List[float]] = collections.defaultdict(list)
    for value in selected:
        by_round[int(value.get("round_index", 0))].append(
            float(value.get("total_analysis_ms", 0.0))
        )
        by_case[str(value.get("case_id", ""))].append(
            float(value.get("total_analysis_ms", 0.0))
        )
    round_medians = {
        str(round_index): round(statistics.median(values), 6)
        for round_index, values in sorted(by_round.items())
    }
    case_medians = {
        case_id: round(statistics.median(values), 6)
        for case_id, values in sorted(by_case.items())
    }
    overall_median = statistics.median(timings)
    mean = statistics.mean(timings)
    relative_range = (
        (max(round_medians.values()) - min(round_medians.values())) / overall_median
        if overall_median > 0 and round_medians else None
    )
    round_ratio = (
        max(round_medians.values()) / min(round_medians.values())
        if round_medians and min(round_medians.values()) > 0 else None
    )
    case_ratio = (
        max(case_medians.values()) / min(case_medians.values())
        if case_medians and min(case_medians.values()) > 0 else None
    )
    coefficient = statistics.pstdev(timings) / mean if mean > 0 else None
    advisory_threshold = 1.2
    return {
        "available": True,
        "reason": "",
        "normalized_manifest_sha256": digest,
        "case_ids": sorted({str(value.get("case_id", "")) for value in selected}),
        "sample_count": len(selected),
        "measured_round_count": len(round_medians),
        "round_median_analysis_time_ms": round_medians,
        "case_median_analysis_time_ms": case_medians,
        "overall_median_analysis_time_ms": round(overall_median, 6),
        "round_median_max_over_min": round(round_ratio, 9) if round_ratio is not None else None,
        "case_median_max_over_min": round(case_ratio, 9) if case_ratio is not None else None,
        "round_relative_range_overall_median": round(relative_range, 9) if relative_range is not None else None,
        "coefficient_of_variation": round(coefficient, 9) if coefficient is not None else None,
        "advisory_max_over_min_threshold": advisory_threshold,
        "status": (
            "WithinAdvisoryThreshold"
            if round_ratio is not None and case_ratio is not None
            and round_ratio <= advisory_threshold and case_ratio <= advisory_threshold
            else "ExceedsAdvisoryThreshold"
        ),
        "interpretation": "advisory measurement-quality control only; it is not a semantic test result",
    }


def _sample_metrics(manifest_path: pathlib.Path, metrics_path: pathlib.Path, process_path: pathlib.Path) -> Mapping[str, Any]:
    manifest = load_json(manifest_path)
    # The standalone driver intentionally stops at the transpiler boundary.
    # A trustworthy artifact binding requires Native-engine module and
    # intermediate hashes, so never fabricate one for timing-only manifests.
    # Real benchmark manifests still use strict binding validation.
    validate_manifest(manifest, require_artifact_binding=False)
    if "artifact_binding" in manifest:
        raise EvaluationError(
            "standalone transpiler driver unexpectedly emitted artifact_binding; "
            "only the Native engine may create a bound artifact identity"
        )
    profile = load_json(metrics_path)
    process = load_json(process_path)
    if int(profile.get("schema_version", 0)) != 1:
        raise EvaluationError("analysis metrics schema_version must be 1")
    if profile.get("status") != "Compiled":
        raise EvaluationError(
            "analysis driver did not compile the sample: %s" % profile.get("reason", "")
        )
    if not bool(profile.get("profiling_enabled")):
        raise EvaluationError("analysis driver was built without enabled profiling")
    build_features = profile.get("build_features")
    if not isinstance(build_features, Mapping):
        raise EvaluationError("analysis metrics omit compiler build_features")
    normalized_features = {
        key: build_features.get(key) for key in REQUIRED_SCALABILITY_BUILD_FEATURES
    }
    if normalized_features != REQUIRED_SCALABILITY_BUILD_FEATURES:
        raise EvaluationError(
            "scalability driver build-feature mismatch: expected %s, got %s"
            % (REQUIRED_SCALABILITY_BUILD_FEATURES, normalized_features)
        )
    if int(process.get("schema_version", 0)) != 1:
        raise EvaluationError("process metrics schema_version must be 1")
    if process.get("returncode") != 0 or bool(process.get("timed_out")) or process.get("launch_error"):
        raise EvaluationError("analysis process did not exit cleanly")
    if not isinstance(process.get("peak_rss_kib"), int) or int(process.get("peak_rss_kib")) <= 0:
        raise EvaluationError("process metrics contain no positive peak_rss_kib")
    refinement = manifest.get("refinement", {})
    obligations = [value for value in refinement.get("proof_obligations", []) if isinstance(value, Mapping)]
    solver_status = collections.Counter()
    solver_time = 0.0
    solver_invocations = 0
    solver_assumption_references = 0
    unique_solver_assumptions: Set[str] = set()
    for obligation in obligations:
        result = obligation.get("solver_result", {})
        if not isinstance(result, Mapping):
            continue
        status = str(result.get("status", "NotRun"))
        solver_status[status] += 1
        elapsed = float(result.get("elapsed_time_ms", 0) or 0)
        solver_time += elapsed
        if status in SOLVER_CALLED_STATUSES:
            solver_invocations += 1
            raw_assumptions = result.get("assumption_constraint_ids", [])
            if not isinstance(raw_assumptions, list):
                raise EvaluationError(
                    "solver_result.assumption_constraint_ids must be an array when the solver was called"
                )
            assumption_ids = [str(value) for value in raw_assumptions]
            solver_assumption_references += len(assumption_ids)
            unique_solver_assumptions.update(assumption_ids)
        if status == "Unknown" and any(token in str(result.get("reason", "")).lower() for token in ("timeout", "timed out", "canceled")):
            solver_status["Timeout"] += 1
    manifest_bytes = manifest_path.stat().st_size
    certificate_bytes = len(json.dumps(manifest.get("parallel_certificate", {}), sort_keys=True, separators=(",", ":")).encode("utf-8"))
    formula_projection = _formula_ir_projection(refinement)
    formula_bytes = len(json.dumps(formula_projection, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    normalized_manifest = _normalized_manifest(manifest)
    normalized_bytes = len(json.dumps(normalized_manifest, sort_keys=True, separators=(",", ":")).encode("utf-8"))
    normalized_certificate_bytes = len(json.dumps(normalized_manifest.get("parallel_certificate", {}), sort_keys=True, separators=(",", ":")).encode("utf-8"))
    phase_times = profile.get("phase_times_ms", {}) if isinstance(profile.get("phase_times_ms"), Mapping) else {}
    missing_phases = sorted(ANALYSIS_PHASES.difference(phase_times))
    if missing_phases:
        raise EvaluationError("analysis metrics omit phases: %s" % ",".join(missing_phases))
    try:
        parsed_phase_times = {str(key): float(value) for key, value in phase_times.items()}
        total_analysis_ms = float(profile["total_analysis_ms"])
    except (KeyError, TypeError, ValueError) as exc:
        raise EvaluationError("analysis metrics contain non-numeric timing data: %s" % exc)
    if not math.isfinite(total_analysis_ms) or total_analysis_ms < 0 or any(
        not math.isfinite(value) or value < 0 for value in parsed_phase_times.values()
    ):
        raise EvaluationError("analysis metrics contain non-finite or negative timing data")
    return {
        "manifest": manifest,
        "profile": profile,
        "process": process,
        "metrics": {
            "phase_times_ms": parsed_phase_times,
            "total_analysis_ms": total_analysis_ms,
            "manifest_binding_mode": "analysis_only_unbound",
            "driver_build_features": dict(normalized_features),
            "driver_build_features_sha256": _canonical_sha256(normalized_features),
            "process_wall_time_ms": float(process.get("wall_time_ms", 0)),
            "peak_rss_kib": int(process.get("peak_rss_kib", 0)),
            # Compatibility alias: these are persisted FormulaIR constraints,
            # not the number of assertions sent to an SMT solver.
            "constraint_count": len(refinement.get("constraints", [])),
            "formula_ir_constraint_count": len(refinement.get("constraints", [])),
            "obligation_count": len(obligations),
            "solver_invocation_count": solver_invocations,
            "solver_goal_count": solver_invocations,
            "solver_assumption_reference_count": solver_assumption_references,
            "solver_unique_assumption_constraint_count": len(unique_solver_assumptions),
            "solver_time_ms": solver_time,
            "solver_status": dict(sorted(solver_status.items())),
            "manifest_size_bytes": manifest_bytes,
            "certificate_size_bytes": certificate_bytes,
            "normalized_certificate_size_bytes": normalized_certificate_bytes,
            "formula_ir_size_bytes": formula_bytes,
            "normalized_manifest_size_bytes": normalized_bytes,
            "normalized_manifest_sha256": _canonical_sha256(normalized_manifest),
            "formula_ir_sha256": _canonical_sha256(formula_projection),
            "formula_ast_nodes": _formula_nodes(formula_projection),
            "cfg_node_count": sum(len(value.get("nodes", [])) for value in manifest.get("control_flow", {}).get("functions", []) if isinstance(value, Mapping)),
        },
    }


def run_scalability(
    config: Mapping[str, Any],
    config_path: pathlib.Path,
    output_root: pathlib.Path,
    work_root: pathlib.Path,
    analysis_driver: pathlib.Path,
    warmups: Optional[int],
    repetitions: Optional[int],
) -> Mapping[str, Any]:
    defaults = config.get("defaults", {}) if isinstance(config.get("defaults"), Mapping) else {}
    sample_timeout = float(defaults.get("timeout_seconds", 60))
    warmup_count = int(defaults.get("warmup_repetitions", 1) if warmups is None else warmups)
    measured_count = int(defaults.get("measured_repetitions", 5) if repetitions is None else repetitions)
    schedule_seed = int(defaults.get("seed", 88))
    if measured_count <= 0 or warmup_count < 0:
        raise EvaluationError("repetitions must be positive and warmups non-negative")
    if not analysis_driver.is_file():
        raise EvaluationError("analysis driver does not exist: %s" % analysis_driver)
    worker = HERE / "MeasurementWorker.py"
    points: List[Mapping[str, Any]] = []
    all_samples: List[Mapping[str, Any]] = []
    configured_cases = _case_configurations(config)
    base_configuration = dict(config.get("scalability", {}).get("base", {}))
    states: Dict[str, Dict[str, Any]] = {}
    preparation_failures: Dict[str, Mapping[str, Any]] = {}

    # Generate and validate the source configurations before starting any
    # timed round.  This prevents generator work from being interleaved with
    # compiler measurements and gives every live case the same round count.
    for case_id, axis, case in configured_cases:
        case_directory = work_root / case_id
        case_directory.mkdir(parents=True, exist_ok=True)
        try:
            source, generator_metadata = generate_program(case)
        except GeneratorConfigurationError as exc:
            preparation_failures[case_id] = {
                "case_id": case_id,
                "sweep_axis": axis,
                "status": "ConfigurationError",
                "reason": str(exc),
                "requested": dict(case),
            }
            continue
        source_path = case_directory / (generator_metadata["contract"] + ".prd")
        source_path.write_text(source, encoding="utf-8")
        _write_json(case_directory / "generator_metadata.json", generator_metadata)
        requested = generator_metadata["requested"]
        states[case_id] = {
            "case_id": case_id,
            "axis": axis,
            "case_directory": case_directory,
            "source_path": source_path,
            "generator_metadata": generator_metadata,
            "samples": [],
            "failure_reason": "",
            "baseline_configuration": all(
                int(requested.get(key, -1)) == int(value)
                for key, value in base_configuration.items()
            ),
        }

    schedule = _build_round_schedule(
        list(states), warmup_count, measured_count, schedule_seed
    )
    for scheduled_round in schedule:
        round_kind = str(scheduled_round["round_kind"])
        round_index = int(scheduled_round["round_index"])
        for order_in_round, case_id in enumerate(scheduled_round["case_order"]):
            state = states[case_id]
            if state["failure_reason"]:
                continue
            sample_directory = state["case_directory"] / (
                ("warmup_%02d" if round_kind == "warmup" else "sample_%02d")
                % round_index
            )
            sample_directory.mkdir(parents=True, exist_ok=True)
            manifest_path = sample_directory / "relay_protocol.json"
            metrics_path = sample_directory / "analysis_metrics.json"
            process_path = sample_directory / "process_metrics.json"
            command = [
                sys.executable, str(worker), "--cwd", str(analysis_driver.parent), "--output", str(process_path),
                "--timeout-seconds", str(sample_timeout), "--",
                str(analysis_driver), "--source", str(state["source_path"]), "--manifest", str(manifest_path), "--metrics", str(metrics_path),
            ]
            completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if completed.returncode != 0 or not manifest_path.is_file() or not metrics_path.is_file() or not process_path.is_file():
                process_reason = ""
                if process_path.is_file():
                    failed_process = load_json(process_path)
                    process_reason = "timed_out=%s launch_error=%s stderr=%s" % (
                        failed_process.get("timed_out", False),
                        failed_process.get("launch_error", ""),
                        str(failed_process.get("stderr", ""))[-500:],
                    )
                state["failure_reason"] = "analysis worker failed (returncode=%s): %s %s" % (
                    completed.returncode, completed.stderr[-500:], process_reason
                )
                continue
            sample = _sample_metrics(manifest_path, metrics_path, process_path)
            validation = _validate_synthetic(
                sample["manifest"], state["generator_metadata"]
            )
            if not validation["valid"]:
                state["failure_reason"] = (
                    "generated structure validation failed: %s"
                    % validation["mismatches"]
                )
                continue
            if round_kind == "measured":
                generator_metadata = state["generator_metadata"]
                record = {
                    "case_id": case_id,
                    "sweep_axis": state["axis"],
                    "repeat": round_index,
                    "round_kind": round_kind,
                    "round_index": round_index,
                    "global_round": int(scheduled_round["round"]),
                    "round_seed": int(scheduled_round["round_seed"]),
                    "order_in_round": order_in_round,
                    "baseline_configuration": bool(
                        state["baseline_configuration"]
                    ),
                    "coupled_dimensions": COUPLED_SWEEP_DIMENSIONS.get(state["axis"], []),
                    "requested": generator_metadata["requested"], "derived": generator_metadata["derived"],
                    "validation": validation, **sample["metrics"],
                }
                state["samples"].append(record)
                all_samples.append(record)

    # Aggregate only after all rounds have completed.  Consequently the five
    # samples for one case span the full experiment instead of one contiguous
    # time interval, which de-confounds case order from host drift.
    for case_id, axis, _ in configured_cases:
        if case_id in preparation_failures:
            points.append(preparation_failures[case_id])
            continue
        state = states[case_id]
        generator_metadata = state["generator_metadata"]
        samples = state["samples"]
        failure_reason = str(state["failure_reason"])
        if not failure_reason and len(samples) != measured_count:
            failure_reason = "expected %d measured samples, found %d" % (
                measured_count, len(samples)
            )
        if failure_reason:
            points.append({
                "case_id": case_id,
                "sweep_axis": axis,
                "status": "InfrastructureFailure",
                "reason": failure_reason,
                "requested": generator_metadata["requested"],
            })
            continue
        invariant_fields = (
            "constraint_count",
            "formula_ir_constraint_count",
            "obligation_count",
            "solver_goal_count",
            "solver_assumption_reference_count",
            "solver_unique_assumption_constraint_count",
            "normalized_certificate_size_bytes",
            "formula_ir_size_bytes",
            "normalized_manifest_size_bytes",
            "normalized_manifest_sha256",
            "formula_ir_sha256",
            "driver_build_features_sha256",
            "formula_ast_nodes",
            "cfg_node_count",
        )
        drift = {
            field: sorted({sample[field] for sample in samples})
            for field in invariant_fields
            if len({sample[field] for sample in samples}) != 1
        }
        if drift:
            points.append({
                "case_id": case_id,
                "sweep_axis": axis,
                "status": "NondeterministicAnalysisStructure",
                "reason": "structure changed across repetitions",
                "drift": drift,
                "requested": generator_metadata["requested"],
            })
            continue
        phase_names = sorted({name for sample in samples for name in sample["phase_times_ms"]})
        solver_status_samples = [sample["solver_status"] for sample in samples]
        solver_status_names = sorted({name for value in solver_status_samples for name in value})
        solver_status_median = {
            name: statistics.median(value.get(name, 0) for value in solver_status_samples)
            for name in solver_status_names
        }
        solver_status_range = {
            name: {
                "min": min(value.get(name, 0) for value in solver_status_samples),
                "median": solver_status_median[name],
                "max": max(value.get(name, 0) for value in solver_status_samples),
            }
            for name in solver_status_names
        }
        solver_status_consistent = all(value == solver_status_samples[0] for value in solver_status_samples)
        analysis_distribution = _distribution(
            [sample["total_analysis_ms"] for sample in samples]
        )
        wall_distribution = _distribution(
            [sample["process_wall_time_ms"] for sample in samples]
        )
        rss_distribution = _distribution(
            [sample["peak_rss_kib"] for sample in samples], digits=3
        )
        solver_time_distribution = _distribution(
            [sample["solver_time_ms"] for sample in samples]
        )
        manifest_size_distribution = _distribution(
            [sample["manifest_size_bytes"] for sample in samples], digits=3
        )
        certificate_size_distribution = _distribution(
            [sample["certificate_size_bytes"] for sample in samples], digits=3
        )
        point: Dict[str, Any] = {
            "case_id": case_id, "sweep_axis": axis, "status": "Completed", "reason": "",
            "coupled_dimensions": COUPLED_SWEEP_DIMENSIONS.get(axis, []),
            "baseline_configuration": bool(state["baseline_configuration"]),
            "requested": generator_metadata["requested"], "derived": generator_metadata["derived"],
            "actual_functions": samples[0]["validation"]["actual"]["functions"],
            "actual_statements": generator_metadata["derived"]["statements"],
            "actual_relay_sites": samples[0]["validation"]["actual"]["relay_sites"],
            "actual_relay_width": samples[0]["validation"]["actual"]["relay_width"],
            "actual_relay_depth": samples[0]["validation"]["actual"]["relay_depth"],
            "actual_branch_depth": samples[0]["validation"]["actual"]["branch_depth"],
            "actual_loop_depth": samples[0]["validation"]["actual"]["loop_depth"],
            "actual_sync_call_depth": samples[0]["validation"]["actual"]["sync_call_depth"],
            "actual_arguments_per_relay": samples[0]["validation"]["actual"]["arguments_per_relay"],
            "actual_target_expression_terms": samples[0]["validation"]["actual"]["target_expression_terms"],
            "actual_constraint_count": samples[0]["constraint_count"],
            "actual_formula_ir_constraint_count": samples[0]["formula_ir_constraint_count"],
            "actual_obligation_count": samples[0]["obligation_count"],
            "actual_solver_goal_count": samples[0]["solver_goal_count"],
            "actual_solver_assumption_reference_count": samples[0]["solver_assumption_reference_count"],
            "actual_solver_unique_assumption_constraint_count": samples[0]["solver_unique_assumption_constraint_count"],
            "analysis_time_distribution_ms": analysis_distribution,
            "analysis_time_min_ms": analysis_distribution["min"],
            "analysis_time_q1_ms": analysis_distribution["q1"],
            "analysis_time_median_ms": analysis_distribution["median"],
            "analysis_time_q3_ms": analysis_distribution["q3"],
            "analysis_time_max_ms": analysis_distribution["max"],
            "analysis_time_iqr_ms": analysis_distribution["iqr"],
            "analysis_time_p95_ms": analysis_distribution["p95"],
            "process_wall_distribution_ms": wall_distribution,
            "process_wall_median_ms": wall_distribution["median"],
            "peak_rss_distribution_kib": rss_distribution,
            "peak_rss_min_kib": rss_distribution["min"],
            "peak_rss_q1_kib": rss_distribution["q1"],
            "peak_rss_median_kib": rss_distribution["median"],
            "peak_rss_q3_kib": rss_distribution["q3"],
            "peak_rss_max_kib": rss_distribution["max"],
            "peak_rss_iqr_kib": rss_distribution["iqr"],
            "peak_rss_p95_kib": rss_distribution["p95"],
            "solver_obligation_elapsed_sum_distribution_ms": solver_time_distribution,
            "solver_time_median_ms": solver_time_distribution["median"],
            "solver_invocation_count": int(statistics.median(sample["solver_invocation_count"] for sample in samples)),
            "solver_status": solver_status_samples[0] if solver_status_consistent else None,
            "solver_status_median": solver_status_median,
            "solver_status_range": solver_status_range,
            "solver_status_consistent": solver_status_consistent,
            "solver_status_samples": solver_status_samples,
            "manifest_size_bytes": int(manifest_size_distribution["median"]),
            "manifest_size_p95_bytes": manifest_size_distribution["p95"],
            "certificate_size_bytes": int(certificate_size_distribution["median"]),
            "certificate_size_p95_bytes": certificate_size_distribution["p95"],
            "normalized_certificate_size_bytes": samples[0]["normalized_certificate_size_bytes"],
            "formula_ir_size_bytes": samples[0]["formula_ir_size_bytes"],
            "normalized_manifest_size_bytes": samples[0]["normalized_manifest_size_bytes"],
            "normalized_manifest_sha256": samples[0]["normalized_manifest_sha256"],
            "formula_ir_sha256": samples[0]["formula_ir_sha256"],
            "driver_build_features": samples[0]["driver_build_features"],
            "formula_ast_nodes": samples[0]["formula_ast_nodes"],
            "cfg_node_count": samples[0]["cfg_node_count"],
            "phase_time_median_ms": {name: round(statistics.median(sample["phase_times_ms"].get(name, 0.0) for sample in samples), 6) for name in phase_names},
        }
        points.append(point)

    transpiler_library = analysis_driver.parent / "transpiler.so"
    completed_case_ids = {
        str(point.get("case_id"))
        for point in points
        if point.get("status") == "Completed"
    }
    timing_stability = _timing_stability_control(
        [sample for sample in all_samples if sample["case_id"] in completed_case_ids]
    )
    payload = {
        "schema_version": 1,
        "metadata": {
            "generated_at": _utc_now(), "config": _portable(config_path), "config_sha256": _sha256(config_path),
            "analysis_driver": _portable(analysis_driver), "analysis_driver_sha256": _sha256(analysis_driver),
            "linked_transpiler_library": _portable(transpiler_library) if transpiler_library.is_file() else "",
            "linked_transpiler_library_sha256": _sha256(transpiler_library) if transpiler_library.is_file() else "",
            "warmup_repetitions": warmup_count, "measured_repetitions": measured_count,
            "sample_timeout_seconds": sample_timeout,
            "host": {"system": platform.system(), "machine": platform.machine(), "logical_cpus": os.cpu_count()},
            "measurement_note": "fresh driver process per case per round; peak RSS is Linux RUSAGE_CHILDREN ru_maxrss",
            "sampling_schedule": {
                "design": "round_major_deterministic_hash_shuffle",
                "base_seed": schedule_seed,
                "warmup_rounds_precede_measured_rounds": True,
                "fresh_process_per_case_per_round": True,
                "rounds": schedule,
            },
            "descriptive_tail_note": "p95 is a descriptive linear interpolation over n=%d samples; it is not a tail-latency estimate" % measured_count,
            "solver_time_note": "solver_time_ms is the per-manifest sum of persisted obligation elapsed times, not one wall-clock solver phase",
            "formula_constraint_note": "formula_ir_constraint_count includes semantic definitions; solver_goal_count and solver assumption counts identify actual solver-facing volume",
            "timing_stability": timing_stability,
            "manifest_binding_mode": "analysis_only_unbound_at_transpiler_boundary",
        },
        "points": points,
        "samples": all_samples,
        "diagnostics": [point for point in points if point["status"] != "Completed"],
    }
    output = output_root / "scalability"
    _write_json(output / "scalability.json", payload)
    fields = [
        "case_id", "sweep_axis", "coupled_dimensions", "baseline_configuration", "status", "reason", "actual_functions", "actual_statements", "actual_relay_sites", "actual_relay_width", "actual_relay_depth", "actual_branch_depth", "actual_loop_depth", "actual_sync_call_depth", "actual_arguments_per_relay", "actual_target_expression_terms",
        "actual_constraint_count", "actual_formula_ir_constraint_count", "actual_obligation_count", "actual_solver_goal_count", "actual_solver_assumption_reference_count", "actual_solver_unique_assumption_constraint_count",
        "analysis_time_min_ms", "analysis_time_q1_ms", "analysis_time_median_ms", "analysis_time_q3_ms", "analysis_time_max_ms", "analysis_time_iqr_ms", "analysis_time_p95_ms", "process_wall_median_ms",
        "peak_rss_min_kib", "peak_rss_q1_kib", "peak_rss_median_kib", "peak_rss_q3_kib", "peak_rss_max_kib", "peak_rss_iqr_kib", "peak_rss_p95_kib",
        "solver_time_median_ms", "solver_invocation_count", "solver_status_consistent", "solver_proved_min", "solver_proved_median", "solver_proved_max", "solver_disproved_min", "solver_disproved_median", "solver_disproved_max", "solver_unknown_min", "solver_unknown_median", "solver_unknown_max", "solver_timeout_min", "solver_timeout_median", "solver_timeout_max", "manifest_size_bytes", "manifest_size_p95_bytes", "certificate_size_bytes", "certificate_size_p95_bytes", "normalized_certificate_size_bytes", "formula_ir_size_bytes", "normalized_manifest_size_bytes", "formula_ast_nodes", "cfg_node_count",
        "cfg_time_ms", "call_graph_time_ms", "effect_time_ms", "icfg_time_ms", "summary_time_ms", "refinement_time_ms", "certificate_time_ms",
    ]
    with (output / "scalability.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for point in points:
            solver_ranges = point.get("solver_status_range", {})
            phases = point.get("phase_time_median_ms", {})
            writer.writerow({
                **{key: point.get(key, "") for key in fields},
                "coupled_dimensions": json.dumps(point.get("coupled_dimensions", []), separators=(",", ":")),
                **{
                    "solver_%s_%s" % (status.lower(), statistic): solver_ranges.get(label, {}).get(statistic, 0)
                    for status, label in (("proved", "Proved"), ("disproved", "Disproved"), ("unknown", "Unknown"), ("timeout", "Timeout"))
                    for statistic in ("min", "median", "max")
                },
                "cfg_time_ms": phases.get("cfg_construction", phases.get("cfg", "")), "call_graph_time_ms": phases.get("call_graph_construction", phases.get("call_graph", "")),
                "effect_time_ms": phases.get("effect_analysis", phases.get("effect", "")), "icfg_time_ms": phases.get("icfg_construction", phases.get("icfg", "")),
                "summary_time_ms": phases.get("summary_analysis", phases.get("summary_construction", phases.get("summary", ""))), "refinement_time_ms": phases.get("refinement_generation", phases.get("refinement", "")),
                "certificate_time_ms": phases.get("certificate_generation", phases.get("certificate", "")),
            })
    scalability_curve(points, output / "scalability_curve.svg")
    return payload


def main(argv: Sequence[str] = ()) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("real", "scalability", "all"))
    parser.add_argument("--config", type=pathlib.Path, default=DEFAULT_CONFIG)
    parser.add_argument("--output-root", type=pathlib.Path, default=REPO_ROOT / "results")
    parser.add_argument("--work-root", type=pathlib.Path, default=pathlib.Path("/tmp/rpreda-coverage-scalability"))
    parser.add_argument("--chsimu", type=pathlib.Path, default=REPO_ROOT / "bin" / "bin_release" / "chsimu")
    parser.add_argument("--analysis-driver", type=pathlib.Path, default=REPO_ROOT / "bin" / "bin_release" / "rpreda_analysis_driver")
    parser.add_argument("--library-path", action="append", default=[])
    parser.add_argument("--path-prefix", action="append", default=[])
    parser.add_argument("--reuse-artifacts", type=pathlib.Path)
    parser.add_argument("--warmups", type=int)
    parser.add_argument("--repetitions", type=int)
    args = parser.parse_args(list(argv) if argv else None)
    config_path = args.config.expanduser().resolve()
    config = _load_config(config_path)
    args.output_root.mkdir(parents=True, exist_ok=True)
    args.work_root.mkdir(parents=True, exist_ok=True)
    try:
        if args.mode in ("real", "all"):
            run_real_world(config, config_path, args.output_root.resolve(), args.work_root.resolve() / "real", args.chsimu.expanduser().resolve(), args.library_path, args.path_prefix, args.reuse_artifacts.expanduser().resolve() if args.reuse_artifacts else None)
        if args.mode in ("scalability", "all"):
            run_scalability(config, config_path, args.output_root.resolve(), args.work_root.resolve() / "scalability", args.analysis_driver.expanduser().resolve(), args.warmups, args.repetitions)
    except (EvaluationError, ManifestValidationError, GeneratorConfigurationError, OSError, ValueError) as exc:
        print("R-PREDA evaluation failed: %s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
