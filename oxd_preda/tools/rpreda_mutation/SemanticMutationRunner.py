#!/usr/bin/env python3
"""Run the multi-benchmark R-PREDA semantic mutation study.

The runner keeps four detection layers independent.  In particular, it asks
Z3 a cross-version preservation question over baseline and mutant Formula IR;
it never proves a compiler definition by assuming that same definition.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys
from collections import Counter
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Sequence, Tuple

from MutationReport import DETECTED, write_json, write_results
from MutationRunner import (
    DEFAULT_CHSIMU,
    DEFAULT_ENGINE,
    REPO_ROOT,
    CompileResult,
    ProcessResult,
    RunnerError,
    _align_surviving_sites,
    certificate_regressions,
    compile_source,
    inspect_z3,
    mutation_coverage_requirements,
    persist_process,
    portable_path,
    protocol_difference,
    publish_generation_metadata,
    read_json,
    run_process,
    run_readonly,
    run_runtime_strict,
    sha256_file,
    utc_now,
)
from SemanticFormulaComparator import compare_manifest_refinements
from SemanticMutationReport import write_detection_breakdown


HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_CONFIG = HERE / "semantic_benchmarks" / "benchmarks.json"
DEFAULT_OUTPUT = REPO_ROOT / "results" / "mutation_semantic"

SOURCE_MUTATION_KINDS = {
    "TargetArithmeticPerturb",
    "TargetVariableSwap",
    "ArgumentArithmeticPerturb",
    "GuardBoundaryChange",
    "RelayOrderSwap",
    "BroadcastToSingle",
    "IntroduceAlias",
    "RelayDuplicate",
    "IntroduceRelayRecursion",
}

FORMULA_MUTATION_KINDS = {
    "TargetArithmeticPerturb",
    "TargetVariableSwap",
    "ArgumentArithmeticPerturb",
    "GuardBoundaryChange",
}

CERTIFICATE_MUTATION_KINDS = {
    "RelayOrderSwap",
    "BroadcastToSingle",
    "IntroduceAlias",
    "RelayDuplicate",
    "IntroduceRelayRecursion",
}

RUNTIME_FAULT_KINDS = {
    "RuntimeTargetScopeCorruption",
    "RuntimeRelayDuplicate",
}

RUNTIME_FAULT_EXPECTED_CHECKS = {
    "RuntimeTargetScopeCorruption": {"target_scope_kind"},
    "RuntimeRelayDuplicate": {"direct_count", "count_upper_bound"},
}

KIND_CATEGORY = {
    "TargetArithmeticPerturb": "Target",
    "TargetVariableSwap": "Target",
    "ArgumentArithmeticPerturb": "Argument",
    "GuardBoundaryChange": "Guard",
    "RelayOrderSwap": "Order",
    "BroadcastToSingle": "Broadcast",
    "IntroduceAlias": "Alias",
    "RelayDuplicate": "Work",
    "IntroduceRelayRecursion": "Depth",
    "RuntimeTargetScopeCorruption": "Runtime",
    "RuntimeRelayDuplicate": "Runtime",
}

LAYER_ORDER = ("static", "z3", "certificate", "runtime")
PIPELINE_ORDER = (
    "CompilerRejected",
    "StaticProtocolMismatch",
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
)

ABNORMAL_TERMINATIONS = {
    "Timeout",
    "GasUsedUp",
    "LaunchError",
    "Signal",
    "FatalDiagnostic",
    "TraceReportMissing",
    "TraceInstrumentationError",
    "CoverageFailure",
    "UnclassifiedNonZeroExit",
}


class ConfigurationError(RunnerError):
    pass


class GenerationDiagnosticsError(RunnerError):
    def __init__(self, diagnostics: Sequence[str]) -> None:
        self.diagnostics = [str(value) for value in diagnostics]
        super().__init__(
            "mutation generator emitted diagnostics: "
            + "; ".join(self.diagnostics)
        )


@dataclass
class BenchmarkResult:
    records: List[Mapping[str, Any]]
    controls: List[Mapping[str, Any]]
    failed: bool
    summary: Mapping[str, Any]


def _dedupe(values: Iterable[str]) -> List[str]:
    return list(dict.fromkeys(value for value in values if value))


def _stable_id(*parts: Any) -> str:
    encoded = json.dumps(
        list(parts), sort_keys=True, separators=(",", ":"), ensure_ascii=False
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()[:32]


def _choose_classification(
    detections: Iterable[str], unsupported: bool = False
) -> str:
    evidence = set(detections)
    for classification in PIPELINE_ORDER:
        if classification in evidence:
            return classification
    if "InfrastructureFailure" in evidence:
        return "InfrastructureFailure"
    return "Unsupported" if unsupported else "Survived"


def _resolve_path(
    value: str | pathlib.Path,
    config_directory: pathlib.Path,
    *,
    source_controlled: bool,
) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if path.is_absolute():
        return path.resolve()
    candidates = (
        (REPO_ROOT / path, config_directory / path)
        if source_controlled
        else (config_directory / path, REPO_ROOT / path)
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    return candidates[0].resolve()


def load_configuration(path: pathlib.Path) -> Mapping[str, Any]:
    try:
        payload = read_json(path)
    except (OSError, json.JSONDecodeError) as exc:
        raise ConfigurationError(f"cannot read semantic benchmark config: {exc}")
    if not isinstance(payload, Mapping):
        raise ConfigurationError("semantic benchmark config must be an object")
    if int(payload.get("schema_version", 0)) != 1:
        raise ConfigurationError("semantic benchmark config schema_version must be 1")
    benchmarks = payload.get("benchmarks")
    if not isinstance(benchmarks, Mapping) or not benchmarks:
        raise ConfigurationError("semantic benchmark config has no benchmarks")
    for name, raw in benchmarks.items():
        if not isinstance(raw, Mapping):
            raise ConfigurationError(f"benchmark {name!r} must be an object")
        if raw.get("enabled", True) is False:
            continue
        if not raw.get("source") or not raw.get("runtime_template"):
            raise ConfigurationError(
                f"benchmark {name!r} requires source and runtime_template"
            )
        kinds = configured_source_kinds(raw)
        faults = raw.get("runtime_faults", [])
        if not kinds and not faults:
            raise ConfigurationError(
                f"benchmark {name!r} has no explicit source mutation kinds or runtime faults"
            )
        filters = raw.get("mutation_function_filters", {})
        if filters and not isinstance(filters, Mapping):
            raise ConfigurationError(
                f"benchmark {name!r} mutation_function_filters must be an object"
            )
    return payload


def configured_source_kinds(benchmark: Mapping[str, Any]) -> List[str]:
    raw = benchmark.get("mutation_kinds")
    if raw is None:
        # Backward-compatible input for the first benchmark-matrix draft.  The
        # engine still receives an explicit --kinds list; runtime-only names
        # and descriptive aliases are never forwarded.
        raw = benchmark.get("recommended_mutations", [])
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes)):
        raise ConfigurationError("mutation_kinds must be an array")
    values = _dedupe(str(value) for value in raw)
    unknown = sorted(set(values) - SOURCE_MUTATION_KINDS - RUNTIME_FAULT_KINDS)
    if unknown:
        raise ConfigurationError(
            "unknown or non-source mutation kinds: " + ", ".join(unknown)
        )
    return [value for value in values if value in SOURCE_MUTATION_KINDS]


def _signature_patterns(
    benchmark: Mapping[str, Any], mutation_kind: str | None = None
) -> List[str]:
    filters = benchmark.get("mutation_function_filters", {})
    if mutation_kind and isinstance(filters, Mapping) and mutation_kind in filters:
        raw = filters.get(mutation_kind, [])
    else:
        coverage = benchmark.get("coverage_allowlist", {})
        raw = (
            coverage.get("portable_source_function_signatures", [])
            if isinstance(coverage, Mapping)
            else []
        )
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes)):
        raise ConfigurationError("function signature filter must be an array")
    return _dedupe(str(value) for value in raw)


def _matches_signature(value: str, patterns: Sequence[str]) -> bool:
    return not patterns or any(pattern in value for pattern in patterns)


def _relay_sites(manifest: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    return [
        site
        for site in manifest.get("relay_sites", [])
        if isinstance(site, Mapping)
    ]


def coverage_requirements(
    manifest: Mapping[str, Any],
    benchmark_name: str,
    benchmark: Mapping[str, Any],
) -> Mapping[str, Any]:
    patterns = _signature_patterns(benchmark)
    sites = [
        site
        for site in _relay_sites(manifest)
        if _matches_signature(
            str(
                site.get("source_function_signature")
                or site.get("source_function_id", "")
            ),
            patterns,
        )
        or _matches_signature(str(site.get("source_function_id", "")), patterns)
    ]
    if patterns:
        unmatched = [
            pattern
            for pattern in patterns
            if not any(
                pattern
                in str(
                    site.get("source_function_signature")
                    or site.get("source_function_id", "")
                )
                or pattern in str(site.get("source_function_id", ""))
                for site in sites
            )
        ]
        if unmatched:
            raise ConfigurationError(
                f"benchmark {benchmark_name!r} coverage signatures matched no relay site: "
                + ", ".join(unmatched)
            )
    if not sites:
        raise ConfigurationError(
            f"benchmark {benchmark_name!r} coverage allowlist selected no relay sites"
        )
    return {
        "label": benchmark_name,
        "required_function_ids": sorted(
            {str(site.get("source_function_id", "")) for site in sites}
        ),
        "required_site_ids": sorted(str(site.get("id", "")) for site in sites),
        "unresolved_site_ids": [],
    }


def runtime_arguments(
    defaults: Mapping[str, Any], benchmark: Mapping[str, Any], seed: int
) -> List[str]:
    order = int(benchmark.get("order", defaults.get("order", 2)))
    parameters = benchmark.get("runtime_parameters", {})
    if not isinstance(parameters, Mapping):
        raise ConfigurationError("runtime_parameters must be an object")
    arguments = [f"-seed:{seed}", f"-order:{order}"]
    for key in sorted(parameters):
        arguments.append(f"-{key}:{parameters[key]}")
    extra = benchmark.get("runtime_args", [])
    if not isinstance(extra, Sequence) or isinstance(extra, (str, bytes)):
        raise ConfigurationError("runtime_args must be an array")
    arguments.extend(str(value) for value in extra)
    return arguments


def _max_selected(
    benchmark: Mapping[str, Any],
    kind: str,
    defaults: Mapping[str, Any] | None = None,
) -> int:
    raw = benchmark.get(
        "max_selected_per_kind",
        (defaults or {}).get("max_selected_per_kind", 1),
    )
    if isinstance(raw, Mapping):
        raw = raw.get(kind, raw.get("default", 1))
    try:
        value = int(raw)
    except (TypeError, ValueError) as exc:
        raise ConfigurationError("max_selected_per_kind must be an integer") from exc
    if value <= 0:
        raise ConfigurationError("max_selected_per_kind must be positive")
    return value


def validate_generation_index(index: Mapping[str, Any]) -> None:
    diagnostics = index.get("diagnostics", [])
    if not isinstance(diagnostics, Sequence) or isinstance(
        diagnostics, (str, bytes)
    ):
        raise GenerationDiagnosticsError(["top-level diagnostics is not an array"])
    if diagnostics:
        raise GenerationDiagnosticsError([str(value) for value in diagnostics])
    mutations = index.get("mutations")
    if not isinstance(mutations, Sequence) or isinstance(mutations, (str, bytes)):
        raise GenerationDiagnosticsError(["top-level mutations is not an array"])


def select_generated_mutations(
    index: Mapping[str, Any],
    benchmark: Mapping[str, Any],
    defaults: Mapping[str, Any] | None = None,
) -> Tuple[List[Mapping[str, Any]], List[str]]:
    validate_generation_index(index)
    configured = configured_source_kinds(benchmark)
    selected: List[Mapping[str, Any]] = []
    counts: Counter[str] = Counter()
    for generated in index.get("mutations", []):
        if not isinstance(generated, Mapping):
            raise GenerationDiagnosticsError(["mutation index contains a non-object row"])
        kind = str(generated.get("mutation_type", ""))
        if kind not in configured or generated.get("generation_status") != "Generated":
            continue
        function_id = str(generated.get("source_function_id", ""))
        if not _matches_signature(function_id, _signature_patterns(benchmark, kind)):
            continue
        if counts[kind] >= _max_selected(benchmark, kind, defaults):
            continue
        selected.append(generated)
        counts[kind] += 1
    missing = [kind for kind in configured if counts[kind] == 0]
    return selected, missing


def _portable_generated(
    generated: Mapping[str, Any], output_root: pathlib.Path
) -> Dict[str, Any]:
    result = dict(generated)
    for field in ("original_code_path", "mutated_code_path"):
        if result.get(field):
            result[field] = portable_path(str(result[field]), output_root)
    return result


def _z3_status(refinement: Mapping[str, Any]) -> str:
    statuses = refinement.get("statuses", {})
    if refinement.get("z3_disproved"):
        return "Disproved"
    for status in (
        "EncodingError",
        "InconsistentAssumptions",
        "NotRun",
        "Unknown",
        "Unsupported",
        "Proved",
    ):
        if int((statuses or {}).get(status, 0) or 0) > 0:
            return status
    return "NotApplicable"


def _runtime_is_abnormal(runtime: Mapping[str, Any] | None) -> bool:
    if not runtime:
        return False
    termination = str(runtime.get("termination_reason", ""))
    if termination in ABNORMAL_TERMINATIONS:
        return True
    if str(runtime.get("status", "")) == "InfrastructureFailure":
        return True
    return bool(runtime.get("infrastructure_failures"))


def runtime_detection_evidence(
    runtime: Mapping[str, Any] | None,
) -> Mapping[str, Any]:
    if runtime is None:
        return {
            "compiler_rejected": False,
            "certificate": False,
            "runtime": False,
            "infrastructure": False,
            "reasons": [],
        }
    if str(runtime.get("status", "")) == "CompilerRejected":
        return {
            "compiler_rejected": True,
            "certificate": False,
            "runtime": False,
            "infrastructure": False,
            "reasons": [
                str(runtime.get("reason", ""))
                or "runtime fixture compilation rejected the source"
            ],
        }
    if _runtime_is_abnormal(runtime):
        reason = str(runtime.get("reason", "")) or str(
            runtime.get("termination_reason", "runtime infrastructure failure")
        )
        return {
            "compiler_rejected": False,
            "certificate": False,
            "runtime": False,
            "infrastructure": True,
            "reasons": [reason],
        }
    certificate = bool(runtime.get("certificate_mismatches"))
    ordinary = bool(runtime.get("runtime_mismatches"))
    return {
        "compiler_rejected": False,
        "certificate": certificate,
        "runtime": ordinary,
        "infrastructure": False,
        "reasons": [],
    }


def _formula_infrastructure(refinement: Mapping[str, Any]) -> List[str]:
    result: List[str] = []
    for obligation in refinement.get("obligations", []):
        solver = obligation.get("solver_result", {})
        status = str(solver.get("status", ""))
        if status in {
            "EncodingError",
            "InconsistentAssumptions",
            "NotRun",
        }:
            result.append(
                f"{obligation.get('id', 'semantic obligation')}: {status}: "
                f"{solver.get('reason', '')}"
            )
    return result


def build_source_record(
    benchmark_name: str,
    generated: Mapping[str, Any],
    aggregate_mutation_id: str,
    compile_result: CompileResult,
    baseline_manifest: Mapping[str, Any],
    z3_binary: pathlib.Path,
    runtime: Mapping[str, Any] | None,
    output_root: pathlib.Path,
) -> Mapping[str, Any]:
    portable = _portable_generated(generated, output_root)
    portable["engine_mutation_id"] = str(generated.get("mutation_id", ""))
    portable["mutation_id"] = aggregate_mutation_id
    mutation_type = str(generated.get("mutation_type", ""))
    category = KIND_CATEGORY.get(mutation_type, "Uncategorized")
    detected_by: List[str] = []
    layers: List[str] = []
    reasons: List[str] = []
    unsupported_stages: List[str] = []
    stage_failures: List[str] = []
    protocol: Mapping[str, Any] = {"changed": False}
    refinement: Mapping[str, Any] = {
        "applicable": mutation_type in FORMULA_MUTATION_KINDS,
        "formula_ir_changed": False,
        "statuses": {},
        "obligations": [],
        "z3_disproved": [],
        "unsupported": [],
    }
    certificate: Mapping[str, Any] = {
        "static_regressions": [],
        "runtime_mismatches": list(
            (runtime or {}).get("certificate_mismatches", [])
        ),
    }

    if compile_result.status == "CompilerRejected":
        detected_by.append("CompilerRejected")
        reasons.append(compile_result.reason)
    elif compile_result.status != "Compiled" or compile_result.manifest is None:
        detected_by.append("InfrastructureFailure")
        stage_failures.append("Compile")
        reasons.append(compile_result.reason or "mutant compilation did not complete")
    else:
        mutant_manifest = compile_result.manifest
        protocol = protocol_difference(baseline_manifest, mutant_manifest)
        if protocol.get("changed"):
            detected_by.append("StaticProtocolMismatch")
            layers.append("static")
            reasons.append("canonical relay protocol differs from baseline")
        alignment = _align_surviving_sites(
            baseline_manifest, mutant_manifest, generated
        )
        refinement = compare_manifest_refinements(
            baseline_manifest,
            mutant_manifest,
            generated,
            alignment,
            z3_binary,
        )
        if refinement.get("z3_disproved"):
            detected_by.append("Z3Disproved")
            layers.append("z3")
            reasons.append("cross-version Formula IR preservation was disproved")
        if refinement.get("unsupported"):
            unsupported_stages.append("Z3Preservation")
        formula_failures = _formula_infrastructure(refinement)
        if formula_failures:
            detected_by.append("InfrastructureFailure")
            stage_failures.append("Z3Preservation")
            reasons.extend(formula_failures)
        regressions = certificate_regressions(
            baseline_manifest, mutant_manifest, generated
        )
        certificate = {
            "static_regressions": regressions,
            "runtime_mismatches": list(
                (runtime or {}).get("certificate_mismatches", [])
            ),
        }
        if regressions:
            detected_by.append("CertificateViolation")
            layers.append("certificate")
            reasons.extend(regressions)

    runtime_evidence = runtime_detection_evidence(runtime)
    if runtime_evidence["compiler_rejected"]:
        detected_by.append("CompilerRejected")
        reasons.extend(runtime_evidence["reasons"])
    elif runtime_evidence["infrastructure"]:
        detected_by.append("InfrastructureFailure")
        stage_failures.append("Runtime")
        reasons.extend(runtime_evidence["reasons"])
    else:
        if runtime_evidence["certificate"]:
            detected_by.append("CertificateViolation")
            layers.append("certificate")
        if runtime_evidence["runtime"]:
            detected_by.append("RuntimeStrictMismatch")
            layers.append("runtime")

    detected_by = _dedupe(detected_by)
    layers = [layer for layer in LAYER_ORDER if layer in set(layers)]
    unsupported = bool(unsupported_stages)
    final = _choose_classification(detected_by, unsupported)
    eligible = {"static", "runtime"}
    if mutation_type in FORMULA_MUTATION_KINDS:
        eligible.add("z3")
    if mutation_type in CERTIFICATE_MUTATION_KINDS:
        eligible.add("certificate")
    return {
        **portable,
        "benchmark": benchmark_name,
        "semantic_category": category,
        "mutation_domain": "source",
        "source_unchanged": False,
        "detection_method": final if final in DETECTED else "",
        "final_classification": final,
        "detected_by": detected_by,
        "detection_layers": layers,
        "ineligible_layers": [layer for layer in LAYER_ORDER if layer not in eligible],
        "compile_status": compile_result.status,
        "static_protocol_changed": bool(protocol.get("changed", False)),
        "z3_status": _z3_status(refinement),
        "certificate_status": "Violation"
        if certificate.get("static_regressions")
        or certificate.get("runtime_mismatches")
        else "NoRegression",
        "runtime_status": (runtime or {}).get("status", "NotRun"),
        "analysis_complete": not stage_failures and not unsupported_stages,
        "stage_failures": _dedupe(stage_failures),
        "unsupported_stages": _dedupe(unsupported_stages),
        "reason": "; ".join(_dedupe(reasons)),
        "analysis": {
            "protocol": protocol,
            "refinement": refinement,
            # Compatibility alias: semantic reports and old consumers can use
            # either name while schema-v1 mutation metrics remain intact.
            "z3": refinement,
            "certificate": certificate,
            "runtime": runtime or {"status": "NotRun"},
            "compile_manifest_path": portable_path(
                compile_result.manifest_path, output_root, retained=False
            ),
        },
    }


def _trace_mismatches(report: Mapping[str, Any]) -> Mapping[str, Any]:
    certificate: List[Mapping[str, Any]] = []
    ordinary: List[Mapping[str, Any]] = []
    for result in report.get("validation_results", []):
        if not isinstance(result, Mapping) or result.get("status") != "Mismatch":
            continue
        detail = result.get("detail", {})
        if not isinstance(detail, Mapping):
            detail = {}
        item = {
            "check_kind": str(
                result.get("check_kind") or detail.get("check_kind", "")
            ),
            "property_id": str(detail.get("property_id", "")),
            "certificate_id": str(detail.get("certificate_id", "")),
            "relay_site_id": str(detail.get("relay_site_id", "")),
            "reason": str(result.get("reason", "")),
        }
        if item["certificate_id"] or item["check_kind"].startswith("certificate_"):
            certificate.append(item)
        else:
            ordinary.append(item)
    return {"certificate": certificate, "runtime": ordinary}


def inspect_fault_application(
    report: Mapping[str, Any], mutation_id: str
) -> Mapping[str, Any]:
    applications: List[Mapping[str, Any]] = []
    for result in report.get("validation_results", []):
        if not isinstance(result, Mapping):
            continue
        detail = result.get("detail", {})
        if not isinstance(detail, Mapping):
            detail = {}
        if str(detail.get("property_id", "")) != f"runtime_fault.{mutation_id}":
            continue
        applications.append(
            {
                "status": str(result.get("status", "")),
                "actual": str(detail.get("actual", "")),
                "reason": str(
                    detail.get("diagnostic_reason") or result.get("reason", "")
                ),
                "relay_site_id": str(detail.get("relay_site_id", "")),
            }
        )
    # Accept a future dedicated top-level representation without weakening the
    # current validation-result provenance requirement.
    for item in report.get("fault_applications", []):
        if not isinstance(item, Mapping):
            continue
        if str(item.get("mutation_id", "")) != mutation_id:
            continue
        applications.append(dict(item))
    actuals = {str(item.get("actual", "")) for item in applications}
    if "Applied" in actuals:
        status = "Applied"
    elif "Invalid" in actuals:
        status = "Invalid"
    elif "NotApplied" in actuals:
        status = "NotApplied"
    else:
        status = "Missing"
    return {"status": status, "records": applications}


def _resolve_fault_site(
    manifest: Mapping[str, Any], raw: Mapping[str, Any]
) -> Tuple[Mapping[str, Any] | None, str]:
    selector = raw.get("selector", {})
    if not isinstance(selector, Mapping):
        return None, "fault selector must be an object"
    function_id = str(selector.get("source_function_id", ""))
    signature = str(
        selector.get("source_function_signature")
        or raw.get("source_function_signature", "")
    )
    site_id = str(selector.get("relay_site_id", ""))
    candidates = []
    for site in _relay_sites(manifest):
        if function_id and str(site.get("source_function_id", "")) != function_id:
            continue
        if signature and not _matches_signature(
            str(site.get("source_function_signature", "")), [signature]
        ) and not _matches_signature(str(site.get("source_function_id", "")), [signature]):
            continue
        if site_id and str(site.get("id", "")) != site_id:
            continue
        candidates.append(site)
    if len(candidates) != 1:
        return None, f"fault selector resolved {len(candidates)} relay sites"
    return candidates[0], ""


def materialize_fault_spec(
    benchmark_name: str,
    raw: Mapping[str, Any],
    baseline_manifest: Mapping[str, Any],
    seed: int,
    path: pathlib.Path,
) -> Tuple[Mapping[str, Any] | None, str]:
    kind = str(raw.get("kind") or raw.get("mutation_type", ""))
    if kind not in RUNTIME_FAULT_KINDS:
        return None, f"unsupported runtime fault kind {kind!r}"
    site, error = _resolve_fault_site(baseline_manifest, raw)
    if site is None:
        return None, error
    raw_selector = raw.get("selector", {})
    mutation_id = str(raw.get("mutation_id", "")) or _stable_id(
        "runtime-fault",
        benchmark_name,
        kind,
        site.get("source_function_id"),
        site.get("id"),
        raw.get("replacement_scope", ""),
        seed,
    )
    selector: Dict[str, Any] = {
        "source_function_id": str(site.get("source_function_id", "")),
        "relay_site_id": str(site.get("id", "")),
        "occurrence_index": int(raw_selector.get("occurrence_index", 0)),
    }
    for optional in (
        "source_module_id",
        "root_trace_tx_id",
        "parent_trace_tx_id",
    ):
        if optional in raw_selector:
            selector[optional] = raw_selector[optional]
    spec: Dict[str, Any] = {
        "schema_version": 1,
        "mutation_id": mutation_id,
        "kind": kind,
        "seed": seed,
        "selector": selector,
    }
    if kind == "RuntimeTargetScopeCorruption":
        replacement = str(raw.get("replacement_scope", ""))
        if not replacement:
            return None, "RuntimeTargetScopeCorruption requires replacement_scope"
        spec["replacement_scope"] = replacement
    path.parent.mkdir(parents=True, exist_ok=True)
    write_json(path, spec)
    return spec, ""


def _unsupported_fault_record(
    benchmark_name: str,
    raw: Mapping[str, Any],
    mutation_id: str,
    reason: str,
) -> Mapping[str, Any]:
    kind = str(raw.get("kind") or raw.get("mutation_type", "RuntimeFault"))
    return {
        "mutation_id": mutation_id,
        "mutation_type": kind,
        "benchmark": benchmark_name,
        "semantic_category": str(
            raw.get("semantic_category", KIND_CATEGORY.get(kind, "Runtime"))
        ),
        "generation_status": "Unsupported",
        "mutation_domain": "runtime_validation_slice",
        "source_unchanged": True,
        "detection_method": "",
        "final_classification": "Unsupported",
        "detected_by": [],
        "detection_layers": [],
        "ineligible_layers": ["static", "z3", "certificate"],
        "compile_status": "BaselineCompiled",
        "static_protocol_changed": False,
        "z3_status": "NotApplicable",
        "certificate_status": "NotRun",
        "runtime_status": "NotRun",
        "analysis_complete": False,
        "stage_failures": [],
        "unsupported_stages": ["RuntimeFaultSelector"],
        "fault_application_status": "UnresolvedSelector",
        "reason": reason,
        "analysis": {
            "protocol": {"changed": False},
            "refinement": {"applicable": False},
            "z3": {"applicable": False},
            "certificate": {"static_regressions": [], "runtime_mismatches": []},
            "runtime": {"status": "NotRun"},
        },
    }


def build_fault_record(
    benchmark_name: str,
    raw: Mapping[str, Any],
    spec: Mapping[str, Any],
    fault_path: pathlib.Path,
    runtime: Mapping[str, Any],
    trace_report: Mapping[str, Any] | None,
    output_root: pathlib.Path,
) -> Mapping[str, Any]:
    mutation_id = str(spec.get("mutation_id", ""))
    kind = str(spec.get("kind", "RuntimeFault"))
    application = inspect_fault_application(trace_report or {}, mutation_id)
    mismatches = _trace_mismatches(trace_report or {})
    expected = raw.get(
        "expected_runtime_check_kinds",
        sorted(RUNTIME_FAULT_EXPECTED_CHECKS.get(kind, set())),
    )
    if not isinstance(expected, Sequence) or isinstance(expected, (str, bytes)):
        expected = []
    expected_set = {str(value) for value in expected}
    actual_kinds = {
        str(item.get("check_kind", ""))
        for item in mismatches["certificate"] + mismatches["runtime"]
    }
    matched_expected = sorted(expected_set.intersection(actual_kinds))
    runtime_evidence = runtime_detection_evidence(runtime)
    applied = application["status"] == "Applied"
    returncode_is_strict_failure = int(runtime.get("process_returncode", 0) or 0) == 2
    expected_mismatch = bool(matched_expected) if expected_set else bool(actual_kinds)
    valid_semantic_run = (
        applied
        and expected_mismatch
        and returncode_is_strict_failure
        and not runtime_evidence["infrastructure"]
        and bool(runtime.get("coverage", {}).get("covered", False))
    )

    detected_by: List[str] = []
    layers: List[str] = []
    reasons: List[str] = []
    stage_failures: List[str] = []
    unsupported_stages: List[str] = []
    if runtime_evidence["compiler_rejected"]:
        detected_by.append("InfrastructureFailure")
        stage_failures.append("RuntimeFault")
        reasons.extend(runtime_evidence["reasons"])
    elif runtime_evidence["infrastructure"]:
        detected_by.append("InfrastructureFailure")
        stage_failures.append("RuntimeFault")
        reasons.extend(runtime_evidence["reasons"])
    elif application["status"] in {"Invalid", "NotApplied", "Missing"}:
        unsupported_stages.append("RuntimeFaultApplication")
        reasons.append(f"runtime fault application status is {application['status']}")
    elif valid_semantic_run:
        if mismatches["certificate"]:
            detected_by.append("CertificateViolation")
            layers.append("certificate")
        if mismatches["runtime"]:
            detected_by.append("RuntimeStrictMismatch")
            layers.append("runtime")
    else:
        unsupported_stages.append("RuntimeExpectedMismatch")
        reasons.append(
            "fault was applied but produced no expected strict mismatch with exit code 2"
        )

    detected_by = _dedupe(detected_by)
    final = _choose_classification(detected_by, bool(unsupported_stages))
    eligible = {"runtime"}
    if kind == "RuntimeRelayDuplicate":
        eligible.add("certificate")
    return {
        "mutation_id": mutation_id,
        "mutation_type": kind,
        "benchmark": benchmark_name,
        "semantic_category": str(
            raw.get("semantic_category", KIND_CATEGORY.get(kind, "Runtime"))
        ),
        "generation_status": "Generated",
        "mutation_domain": "runtime_validation_slice",
        "source_unchanged": True,
        "description": str(raw.get("description", kind)),
        "original_code_path": "",
        "mutated_code_path": "",
        "fault_spec_path": portable_path(fault_path, output_root),
        "expected_runtime_check_kinds": sorted(expected_set),
        "matched_expected_check_kinds": matched_expected,
        "fault_application_status": application["status"],
        "semantic_runtime_detected": valid_semantic_run,
        "detection_method": final if final in DETECTED else "",
        "final_classification": final,
        "detected_by": detected_by,
        "detection_layers": [
            layer for layer in LAYER_ORDER if layer in set(layers)
        ],
        "ineligible_layers": [layer for layer in LAYER_ORDER if layer not in eligible],
        "compile_status": "BaselineCompiled",
        "static_protocol_changed": False,
        "z3_status": "NotApplicable",
        "certificate_status": "Violation"
        if mismatches["certificate"] and valid_semantic_run
        else "NoRegression",
        "runtime_status": runtime.get("status", "NotRun"),
        "analysis_complete": not stage_failures and not unsupported_stages,
        "stage_failures": stage_failures,
        "unsupported_stages": unsupported_stages,
        "reason": "; ".join(_dedupe(reasons)),
        "analysis": {
            "protocol": {"changed": False},
            "refinement": {"applicable": False},
            "z3": {"applicable": False},
            "certificate": {
                "static_regressions": [],
                "runtime_mismatches": mismatches["certificate"],
            },
            "runtime": {
                **runtime,
                "fault_application": application,
                "ordinary_mismatch_details": mismatches["runtime"],
                "certificate_mismatch_details": mismatches["certificate"],
            },
        },
    }


def _failure_control(
    benchmark_name: str, control_id: str, reason: str
) -> Mapping[str, Any]:
    return {
        "control_id": f"{benchmark_name}.{control_id}",
        "benchmark": benchmark_name,
        "final_classification": "InfrastructureFailure",
        "detected_by": ["InfrastructureFailure"],
        "reason": reason,
    }


def _baseline_control(
    benchmark_name: str,
    compile_result: CompileResult,
    runtime: Mapping[str, Any] | None,
) -> Mapping[str, Any]:
    detected: List[str] = []
    reasons: List[str] = []
    stage_failures: List[str] = []
    z3 = inspect_z3(compile_result.manifest or {}, False)
    if compile_result.status == "CompilerRejected":
        detected.append("CompilerRejected")
        reasons.append(compile_result.reason)
    elif compile_result.status != "Compiled" or compile_result.manifest is None:
        detected.append("InfrastructureFailure")
        reasons.append(compile_result.reason or "baseline compilation failed")
    if z3.get("safety_disproved"):
        detected.append("Z3Disproved")
        reasons.append("baseline manifest contains a disproved safety goal")
    if z3.get("infrastructure_failures"):
        # A solver encoding limitation on one baseline obligation does not
        # make the compiled contract or its strict runtime trace invalid.  It
        # disables that analysis slice and is reported explicitly, while the
        # independent static/certificate/runtime layers remain evaluable.
        # A genuinely Disproved baseline safety goal above still fails closed.
        stage_failures.append("Z3Baseline")
        reasons.extend(
            "baseline Z3 analysis incomplete: " + str(value)
            for value in z3["infrastructure_failures"]
        )
    runtime_evidence = runtime_detection_evidence(runtime)
    if runtime_evidence["compiler_rejected"]:
        detected.append("CompilerRejected")
        reasons.extend(runtime_evidence["reasons"])
    elif runtime_evidence["infrastructure"]:
        detected.append("InfrastructureFailure")
        reasons.extend(runtime_evidence["reasons"])
    elif runtime_evidence["certificate"]:
        detected.append("CertificateViolation")
        reasons.append("baseline strict trace violates its certificate")
    elif runtime_evidence["runtime"]:
        detected.append("RuntimeStrictMismatch")
        reasons.append("baseline strict trace contains a runtime mismatch")
    detected = _dedupe(detected)
    return {
        "control_id": f"{benchmark_name}.original_source",
        "benchmark": benchmark_name,
        "final_classification": _choose_classification(detected, False),
        "detected_by": detected,
        "reason": "; ".join(_dedupe(reasons)),
        "compile_status": compile_result.status,
        "analysis_complete": not stage_failures,
        "stage_failures": _dedupe(stage_failures),
        "z3": z3,
        "runtime": runtime or {"status": "NotRun"},
    }


def _source_runtime_requirements(
    generated: Mapping[str, Any],
    baseline_manifest: Mapping[str, Any],
    mutant_manifest: Mapping[str, Any],
) -> Mapping[str, Any]:
    return mutation_coverage_requirements(
        generated, baseline_manifest, mutant_manifest
    )


def run_benchmark(
    name: str,
    benchmark: Mapping[str, Any],
    defaults: Mapping[str, Any],
    config_directory: pathlib.Path,
    output: pathlib.Path,
    engine: pathlib.Path,
    chsimu: pathlib.Path,
    z3_binary: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
    seed: int,
    timeout: float,
) -> BenchmarkResult:
    benchmark_output = output / "benchmarks" / name
    benchmark_output.mkdir(parents=True, exist_ok=True)
    source = _resolve_path(
        str(benchmark["source"]), config_directory, source_controlled=True
    )
    template = _resolve_path(
        str(benchmark["runtime_template"]),
        config_directory,
        source_controlled=False,
    )
    if not source.is_file() or not template.is_file():
        reason = f"missing source or template: {source.name}, {template.name}"
        return BenchmarkResult([], [_failure_control(name, "configuration", reason)], True, {"status": "Failed", "reason": reason})
    arguments = runtime_arguments(defaults, benchmark, seed)
    baseline_compile = compile_source(
        source,
        benchmark_output / "baseline" / "compile",
        chsimu,
        library_paths,
        path_prefixes,
        timeout,
        output,
    )
    baseline_runtime: Mapping[str, Any] | None = None
    baseline_manifest = baseline_compile.manifest
    try:
        requirements = (
            coverage_requirements(baseline_manifest, name, benchmark)
            if baseline_manifest is not None
            else None
        )
    except ConfigurationError as exc:
        control = _failure_control(name, "coverage_configuration", str(exc))
        return BenchmarkResult([], [control], True, {"status": "Failed", "reason": str(exc)})
    if baseline_compile.status == "Compiled" and requirements is not None:
        baseline_runtime = run_runtime_strict(
            source,
            template,
            benchmark_output / "baseline" / "runtime",
            chsimu,
            library_paths,
            path_prefixes,
            timeout,
            arguments,
            requirements,
            output,
        )
    control = _baseline_control(name, baseline_compile, baseline_runtime)
    if control["final_classification"] != "Survived" or baseline_manifest is None:
        return BenchmarkResult([], [control], True, {"status": "Failed", "reason": control.get("reason", "baseline failed")})

    oracle_path = benchmark_output / "baseline" / "oracle.relay_protocol.json"
    write_json(oracle_path, baseline_manifest)
    records: List[Mapping[str, Any]] = []
    controls: List[Mapping[str, Any]] = [control]
    source_kinds = configured_source_kinds(benchmark)
    failed = False
    selected: List[Mapping[str, Any]] = []
    if source_kinds:
        generation_directory = benchmark_output / "generation"
        command = [
            str(engine),
            "--source",
            str(source),
            "--manifest",
            str(oracle_path),
            "--output",
            str(generation_directory),
            "--seed",
            str(seed),
            # Generate the complete deterministic candidate set, then apply
            # benchmark-specific filters before the configured per-kind cap.
            "--max-per-kind",
            "0",
            "--kinds",
            ",".join(source_kinds),
        ]
        generation_process = run_process(command, REPO_ROOT, dict(os.environ), timeout)
        persist_process(
            benchmark_output / "generation_process", generation_process, output
        )
        index_path = generation_directory / "mutation_index.json"
        if (
            generation_process.launch_error
            or generation_process.timed_out
            or generation_process.returncode != 0
            or not index_path.is_file()
        ):
            reason = generation_process.launch_error or "mutation generator failed or produced no index"
            controls.append(_failure_control(name, "mutation_generator", reason))
            failed = True
        else:
            index = read_json(index_path)
            try:
                selected, missing = select_generated_mutations(
                    index, benchmark, defaults
                )
            except GenerationDiagnosticsError as exc:
                controls.append(_failure_control(name, "mutation_generator_diagnostics", str(exc)))
                failed = True
                selected = []
                missing = []
            if not failed and missing:
                reason = "configured mutation kinds selected no candidate: " + ", ".join(missing)
                controls.append(_failure_control(name, "mutation_selection", reason))
                failed = True
                selected = []
            publish_generation_metadata(index, index_path, output)

    for ordinal, generated in enumerate(selected, start=1):
        engine_id = str(generated.get("mutation_id", f"candidate-{ordinal}"))
        aggregate_id = _stable_id("semantic-source", name, engine_id)
        print(f"[{name} source {ordinal}/{len(selected)}] {aggregate_id}", flush=True)
        mutant_source = pathlib.Path(str(generated.get("mutated_code_path", "")))
        run_directory = benchmark_output / "runs" / aggregate_id
        compile_result = compile_source(
            mutant_source,
            run_directory / "compile",
            chsimu,
            library_paths,
            path_prefixes,
            timeout,
            output,
        )
        runtime = None
        if compile_result.status == "Compiled" and compile_result.manifest is not None:
            runtime = run_runtime_strict(
                mutant_source,
                template,
                run_directory / "runtime",
                chsimu,
                library_paths,
                path_prefixes,
                timeout,
                arguments,
                _source_runtime_requirements(
                    generated, baseline_manifest, compile_result.manifest
                ),
                output,
            )
        record = build_source_record(
            name,
            generated,
            aggregate_id,
            compile_result,
            baseline_manifest,
            z3_binary,
            runtime,
            output,
        )
        write_json(run_directory / "result.json", record)
        records.append(record)

    raw_faults = benchmark.get("runtime_faults", [])
    if not isinstance(raw_faults, Sequence) or isinstance(raw_faults, (str, bytes)):
        controls.append(_failure_control(name, "runtime_fault_configuration", "runtime_faults must be an array"))
        failed = True
        raw_faults = []
    for ordinal, raw in enumerate(raw_faults, start=1):
        if not isinstance(raw, Mapping):
            controls.append(_failure_control(name, "runtime_fault_configuration", "runtime fault row must be an object"))
            failed = True
            continue
        provisional_id = str(raw.get("mutation_id", "")) or _stable_id(
            "runtime-fault-invalid", name, ordinal, raw, seed
        )
        run_directory = benchmark_output / "runs" / provisional_id
        spec_path = run_directory / "fault_spec.json"
        spec, error = materialize_fault_spec(
            name, raw, baseline_manifest, seed, spec_path
        )
        if spec is None:
            records.append(
                _unsupported_fault_record(name, raw, provisional_id, error)
            )
            continue
        mutation_id = str(spec["mutation_id"])
        if mutation_id != provisional_id:
            run_directory = benchmark_output / "runs" / mutation_id
            spec_path = run_directory / "fault_spec.json"
            spec, error = materialize_fault_spec(
                name, {**raw, "mutation_id": mutation_id}, baseline_manifest, seed, spec_path
            )
            if spec is None:
                records.append(_unsupported_fault_record(name, raw, mutation_id, error))
                continue
        selector = spec["selector"]
        fault_coverage = {
            "label": mutation_id,
            "required_function_ids": [selector["source_function_id"]],
            "required_site_ids": [selector["relay_site_id"]],
            "unresolved_site_ids": [],
        }
        runtime = run_runtime_strict(
            source,
            template,
            run_directory / "runtime",
            chsimu,
            library_paths,
            path_prefixes,
            timeout,
            arguments,
            fault_coverage,
            output,
            fault_spec_path=spec_path,
        )
        trace_path = run_directory / "runtime" / "trace.json"
        trace_report = None
        if trace_path.is_file():
            try:
                trace_report = read_json(trace_path)
            except (OSError, json.JSONDecodeError):
                trace_report = None
        record = build_fault_record(
            name, raw, spec, spec_path, runtime, trace_report, output
        )
        write_json(run_directory / "result.json", record)
        records.append(record)

    return BenchmarkResult(
        records,
        controls,
        failed,
        {
            "status": (
                "Failed"
                if failed
                else (
                    "CompletedWithAnalysisLimitations"
                    if not control.get("analysis_complete", True)
                    else "Completed"
                )
            ),
            "baseline_analysis_complete": bool(
                control.get("analysis_complete", True)
            ),
            "baseline_stage_failures": list(
                control.get("stage_failures", [])
            ),
            "source": portable_path(source),
            "runtime_template": portable_path(template),
            "source_mutants": len(selected),
            "runtime_fault_mutants": len(raw_faults),
            "record_count": len(records),
        },
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=pathlib.Path, default=DEFAULT_CONFIG)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--engine", type=pathlib.Path, default=DEFAULT_ENGINE)
    parser.add_argument("--chsimu", type=pathlib.Path, default=DEFAULT_CHSIMU)
    parser.add_argument("--z3-bin", type=pathlib.Path)
    parser.add_argument("--library-path", action="append", default=[])
    parser.add_argument("--path-prefix", action="append", default=[])
    parser.add_argument("--seed", type=int)
    parser.add_argument("--timeout", type=float)
    return parser.parse_args(argv)


def _z3_path(value: pathlib.Path | None) -> pathlib.Path:
    if value is not None:
        return value.expanduser().resolve()
    configured = os.environ.get("RPREDA_Z3_BIN", "")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    discovered = shutil.which("z3")
    return pathlib.Path(discovered).resolve() if discovered else pathlib.Path("z3")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    config_path = args.config.expanduser().resolve()
    output = args.output.expanduser().resolve()
    engine = args.engine.expanduser().resolve()
    chsimu = args.chsimu.expanduser().resolve()
    z3_binary = _z3_path(args.z3_bin)
    for label, path in (("config", config_path), ("engine", engine), ("chsimu", chsimu), ("z3", z3_binary)):
        if not path.is_file():
            raise ConfigurationError(f"{label} does not exist: {path}")
    config = load_configuration(config_path)
    defaults = config.get("defaults", {})
    if not isinstance(defaults, Mapping):
        raise ConfigurationError("defaults must be an object")
    seed = int(args.seed if args.seed is not None else defaults.get("seed", 88))
    timeout = float(
        args.timeout
        if args.timeout is not None
        else defaults.get("timeout_seconds", 120)
    )
    if timeout <= 0:
        raise ConfigurationError("timeout must be positive")
    output.mkdir(parents=True, exist_ok=True)

    metadata: Dict[str, Any] = {
        "started_at": utc_now(),
        "config": portable_path(config_path),
        "config_sha256": sha256_file(config_path),
        "seed": seed,
        "timeout_seconds": timeout,
        "chsimu": portable_path(chsimu),
        "chsimu_sha256": sha256_file(chsimu),
        "mutation_engine": portable_path(engine),
        "mutation_engine_sha256": sha256_file(engine),
        "z3_binary": portable_path(z3_binary),
        "z3_binary_sha256": sha256_file(z3_binary),
        "git_commit": run_readonly(["git", "rev-parse", "HEAD"], REPO_ROOT),
        "git_status": run_readonly(
            ["git", "status", "--short", "--untracked-files=no"], REPO_ROOT
        ).splitlines(),
        "git_status_includes_untracked": False,
        "benchmarks": {},
    }
    records: List[Mapping[str, Any]] = []
    controls: List[Mapping[str, Any]] = []
    failed = False
    for name, benchmark in config["benchmarks"].items():
        if benchmark.get("enabled", True) is False:
            continue
        print(f"[benchmark] {name}", flush=True)
        result = run_benchmark(
            str(name),
            benchmark,
            defaults,
            config_path.parent,
            output,
            engine,
            chsimu,
            z3_binary,
            args.library_path,
            args.path_prefix,
            seed,
            timeout,
        )
        records.extend(result.records)
        controls.extend(result.controls)
        failed = failed or result.failed
        metadata["benchmarks"][str(name)] = result.summary

    ids = [str(record.get("mutation_id", "")) for record in records]
    if len(ids) != len(set(ids)):
        raise RunnerError("aggregate mutation IDs are not unique")
    metadata["completed_at"] = utc_now()
    metadata["record_count"] = len(records)
    payload = write_results(output, metadata, records, controls)
    breakdown = write_detection_breakdown(output, records)
    print(
        f"Semantic mutation study complete: {len(records)} mutants, "
        f"detection_rate={payload['metrics']['overall_detection_rate']}, "
        f"multi_layer={breakdown['overall']['multi_layer_mutants']}. "
        f"Results: {output / 'mutation.json'}"
    )
    return 2 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"SemanticMutationRunner: {exc}", file=sys.stderr)
        raise SystemExit(2)
