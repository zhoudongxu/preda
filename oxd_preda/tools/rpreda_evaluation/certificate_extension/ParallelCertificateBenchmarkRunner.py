#!/usr/bin/env python3
"""Run the isolated R-PREDA parallel-certificate extension study.

The runner deliberately reuses the existing mutation-study compiler/runtime
helpers.  Every baseline and mutant is compiled in a fresh HOME, relay sites
are resolved from semantic source attributes (never from listener ordinals),
and negative evidence is computed with the existing
``certificate_regressions`` implementation.

Relative source paths are resolved against ``--repo-root``.  Relative runtime
template paths are resolved against the configuration directory first and the
repository second.  The publishable products are ``certificate_extension.json``
and ``certificate_extension.csv`` below ``--output``.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys
from collections import Counter
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Sequence, Tuple


HERE = pathlib.Path(__file__).resolve().parent
SCHEMA_VERSION = 1
PAIR_RELATIONS = {
    "CoEmissionIndependent",
    "MutuallyExclusive",
    "MustPrecedeAB",
    "MustPrecedeBA",
    "ProvedMayAlias",
}
PAIR_CERTIFICATE_TYPES = {
    "CoEmissionIndependent",
    "MutualExclusive",
    "MutuallyExclusive",
    "MustPrecede",
}
BOUND_CERTIFICATE_TYPES = {"WorkBound", "DepthBound"}
SUPPORTED_MUTATIONS = {
    "IntroduceAlias",
    "RelayOrderSwap",
    "GuardNegate",
    "RelayDuplicate",
}
CSV_FIELDS = (
    "record_id",
    "workload",
    "pattern",
    "certificate_type",
    "source_function_signature",
    "relay_site_a",
    "relay_site_b",
    "status",
    "raw_certificate_status",
    "validation_status",
    "evidence_ids",
    "reason",
    "mutation_mapping",
    "mutation_id",
    "mutation_type",
    "mutation_generation_status",
    "mutation_compile_status",
    "mutation_runtime_status",
    "mutation_detected",
    "mutation_regressions",
)


class BenchmarkConfigurationError(RuntimeError):
    pass


# Internal process failures and public helper validation failures intentionally
# share one exception type.  Keep the short internal name for readability.
BenchmarkError = BenchmarkConfigurationError


def detect_repo_root() -> pathlib.Path:
    configured = os.environ.get("RPREDA_REPO_ROOT", "")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    for anchor in (HERE, pathlib.Path.cwd().resolve()):
        for candidate in (anchor, *anchor.parents):
            if (candidate / "oxd_preda").is_dir() and (candidate / "bin").is_dir():
                return candidate
    return pathlib.Path.cwd().resolve()


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )


def digest_json(value: Any) -> str:
    return hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_text(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(contents, encoding="utf-8")
    temporary.replace(path)


def write_json(path: pathlib.Path, value: Mapping[str, Any]) -> None:
    atomic_text(
        path,
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
    )


def load_json(path: pathlib.Path) -> Mapping[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BenchmarkError(f"cannot read JSON {path}: {exc}") from exc
    if not isinstance(value, Mapping):
        raise BenchmarkError(f"JSON root is not an object: {path}")
    return value


def load_configuration(path: pathlib.Path) -> Mapping[str, Any]:
    """Load and validate the benchmark configuration.

    ``benchmarks`` is the executable schema used by this runner.  ``workloads``
    is an additive compatibility projection for small evidence-only consumers:
    its ``source`` is a logical ``file#entrypoint`` identity, while
    ``source_file`` and ``baseline_source`` retain the real original contract
    path.  No duplicate extension contract is implied or created.
    """

    raw = load_json(path)
    validate_configuration(raw)
    result = json.loads(json.dumps(raw))
    workloads: List[Mapping[str, Any]] = []
    for benchmark in raw.get("benchmarks", []):
        sites = benchmark.get("sites", {})
        expected = []
        for certificate in benchmark.get("certificates", []):
            row = dict(certificate)
            left = str(certificate.get("site_a", ""))
            right = str(certificate.get("site_b", ""))
            row["site_a"] = dict(sites.get(left, {})) if left else {}
            row["site_b"] = dict(sites.get(right, {})) if right else {}
            expected.append(row)
        source = str(benchmark.get("source", ""))
        signature = str(benchmark.get("function_signature", ""))
        workloads.append(
            {
                "id": str(benchmark.get("name", "")),
                "baseline_source": source,
                "source_file": source,
                "source": f"{source}#{signature}",
                "function_signature": signature,
                "runtime_template": str(benchmark.get("runtime_template", "")),
                "expected_certificates": expected,
            }
        )
    result["workloads"] = workloads
    return result


def slug(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_.-")
    return cleaned or "unnamed"


def resolve_repo_path(
    value: str,
    repo_root: pathlib.Path,
    config_directory: pathlib.Path,
    *,
    config_first: bool = False,
) -> pathlib.Path:
    path = pathlib.Path(value).expanduser()
    if path.is_absolute():
        return path.resolve()
    candidates = (
        (config_directory / path, repo_root / path)
        if config_first
        else (repo_root / path, config_directory / path)
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()
    return candidates[0].resolve()


def load_mutation_runner(repo_root: pathlib.Path) -> Any:
    module_directory = repo_root / "oxd_preda" / "tools" / "rpreda_mutation"
    if not module_directory.is_dir():
        raise BenchmarkError(f"mutation tool directory is missing: {module_directory}")
    os.environ["RPREDA_REPO_ROOT"] = str(repo_root)
    sys.path.insert(0, str(module_directory))
    try:
        import MutationRunner  # type: ignore
    except Exception as exc:  # pragma: no cover - environment-specific import
        raise BenchmarkError(f"cannot import MutationRunner: {exc}") from exc
    return MutationRunner


def require_sequence(value: Any, label: str) -> Sequence[Any]:
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes)):
        raise BenchmarkError(f"{label} must be an array")
    return value


def validate_configuration(config: Mapping[str, Any]) -> None:
    if int(config.get("schema_version", 0) or 0) != 1:
        raise BenchmarkError("configuration schema_version must be 1")
    if config.get("aggregate_with_original_coverage") is not False:
        raise BenchmarkError(
            "aggregate_with_original_coverage must be false for this isolated study"
        )
    defaults = config.get("defaults", {})
    if not isinstance(defaults, Mapping):
        raise BenchmarkError("defaults must be an object")
    benchmarks = require_sequence(config.get("benchmarks", []), "benchmarks")
    if not benchmarks:
        raise BenchmarkError("configuration contains no benchmarks")
    names: set[str] = set()
    certificate_ids: set[str] = set()
    for index, raw in enumerate(benchmarks):
        if not isinstance(raw, Mapping):
            raise BenchmarkError(f"benchmarks[{index}] is not an object")
        name = str(raw.get("name", ""))
        if not name or name in names:
            raise BenchmarkError(f"benchmark name is empty or duplicated: {name!r}")
        names.add(name)
        for required in ("source", "runtime_template", "function_signature"):
            if not str(raw.get(required, "")):
                raise BenchmarkError(f"{name}: missing {required}")
        sites = raw.get("sites", {})
        if not isinstance(sites, Mapping) or not sites:
            raise BenchmarkError(f"{name}: sites must be a non-empty object")
        for label, selector in sites.items():
            if not str(label) or not isinstance(selector, Mapping):
                raise BenchmarkError(f"{name}: invalid semantic site selector")
            if "id" in selector or "relay_site_id" in selector:
                raise BenchmarkError(
                    f"{name}.{label}: hard-coded relay site IDs are forbidden"
                )
        certificates = require_sequence(
            raw.get("certificates", []), f"{name}.certificates"
        )
        if not certificates:
            raise BenchmarkError(f"{name}: no certificate expectations")
        for entry in certificates:
            if not isinstance(entry, Mapping):
                raise BenchmarkError(f"{name}: certificate row is not an object")
            record_id = str(entry.get("id", ""))
            if not record_id or record_id in certificate_ids:
                raise BenchmarkError(
                    f"{name}: certificate ID is empty or duplicated: {record_id!r}"
                )
            certificate_ids.add(record_id)
            certificate_type = str(entry.get("certificate_type", ""))
            if certificate_type not in PAIR_CERTIFICATE_TYPES | BOUND_CERTIFICATE_TYPES:
                raise BenchmarkError(
                    f"{name}.{record_id}: unsupported certificate_type {certificate_type!r}"
                )
            if certificate_type in PAIR_CERTIFICATE_TYPES:
                for field in ("site_a", "site_b", "expected_relation"):
                    if not str(entry.get(field, "")):
                        raise BenchmarkError(f"{name}.{record_id}: missing {field}")
                for field in ("site_a", "site_b"):
                    if str(entry[field]) not in sites:
                        raise BenchmarkError(
                            f"{name}.{record_id}: unknown site label {entry[field]!r}"
                        )
            else:
                if not str(entry.get("bound_field", "")):
                    raise BenchmarkError(f"{name}.{record_id}: missing bound_field")
                if "expected_upper_bound" not in entry:
                    raise BenchmarkError(
                        f"{name}.{record_id}: missing expected_upper_bound"
                    )
            mutation = entry.get("mutation", {})
            if not isinstance(mutation, Mapping):
                raise BenchmarkError(f"{name}.{record_id}: mutation must be an object")
            kind = str(mutation.get("kind", ""))
            required = bool(mutation.get("validation_required", False))
            if required and kind not in SUPPORTED_MUTATIONS:
                raise BenchmarkError(
                    f"{name}.{record_id}: unsupported required mutation {kind!r}"
                )
            for label in mutation.get("sites", []):
                if str(label) not in sites:
                    raise BenchmarkError(
                        f"{name}.{record_id}: mutation references unknown site {label!r}"
                    )


def validate_manifest(manifest: Mapping[str, Any]) -> Mapping[str, Any]:
    errors: List[str] = []
    if int(manifest.get("schema_version", 0) or 0) < 5:
        errors.append("schema_version is below 5")
    if not str(manifest.get("contract", "")):
        errors.append("contract is empty")
    binding = manifest.get("artifact_binding", {})
    if not isinstance(binding, Mapping) or binding.get("binding_complete") is not True:
        errors.append("artifact_binding is absent or incomplete")
    sites = manifest.get("relay_sites", [])
    if not isinstance(sites, Sequence) or isinstance(sites, (str, bytes)):
        errors.append("relay_sites is not an array")
        sites = []
    handlers = manifest.get("handlers", [])
    if not isinstance(handlers, Sequence) or isinstance(handlers, (str, bytes)):
        errors.append("handlers is not an array")
    if not isinstance(manifest.get("control_flow"), Mapping):
        errors.append("control_flow is not an object")
    if not isinstance(manifest.get("refinement"), Mapping):
        errors.append("refinement is not an object")
    certificate = manifest.get("parallel_certificate", {})
    if not isinstance(certificate, Mapping) or not isinstance(
        certificate.get("functions"), Sequence
    ):
        errors.append("parallel_certificate.functions is not an array")
        certificate = {"functions": []}
    site_ids = [
        str(site.get("id", "")) for site in sites if isinstance(site, Mapping)
    ]
    if any(not value for value in site_ids):
        errors.append("one or more relay sites have empty IDs")
    if len(site_ids) != len(set(site_ids)):
        errors.append("relay site IDs are not unique")
    known_sites = set(site_ids)
    pair_count = 0
    for function in certificate.get("functions", []):
        if not isinstance(function, Mapping):
            errors.append("parallel certificate function row is not an object")
            continue
        function_id = str(function.get("source_function_id", ""))
        for pair in function.get("pair_relations", []):
            if not isinstance(pair, Mapping):
                errors.append("pair relation row is not an object")
                continue
            pair_count += 1
            left = str(pair.get("site_a", ""))
            right = str(pair.get("site_b", ""))
            if left not in known_sites or right not in known_sites:
                errors.append(f"pair relation references unknown sites: {left}, {right}")
            pair_function = str(pair.get("source_function_id", function_id))
            if function_id and pair_function != function_id:
                errors.append("pair relation source function disagrees with container")
            relation = str(pair.get("relation", ""))
            if relation and relation not in PAIR_RELATIONS:
                errors.append(f"unknown pair relation {relation!r}")
    if errors:
        raise BenchmarkError("invalid fresh manifest: " + "; ".join(errors))
    return {
        "schema_version": int(manifest.get("schema_version", 0)),
        "contract": str(manifest.get("contract", "")),
        "relay_site_count": len(site_ids),
        "pair_relation_count": pair_count,
        "binding_complete": True,
        "module_hash": str(binding.get("module_hash", "")),
    }


def function_matches(site: Mapping[str, Any], signature: str) -> bool:
    return (
        str(site.get("source_function_signature", "")) == signature
        or str(site.get("source_function_id", "")).endswith("::" + signature)
    )


def resolve_function_id(
    manifest: Mapping[str, Any], signature: str
) -> str:
    candidates = {
        str(site.get("source_function_id", ""))
        for site in manifest.get("relay_sites", [])
        if isinstance(site, Mapping) and function_matches(site, signature)
    }
    candidates.discard("")
    if len(candidates) != 1:
        raise BenchmarkError(
            f"function signature {signature!r} resolved to {sorted(candidates)}"
        )
    return next(iter(candidates))


def _site_sort_key(site: Mapping[str, Any]) -> Tuple[int, int, str]:
    location = site.get("location", {})
    if not isinstance(location, Mapping):
        location = {}
    return (
        int(location.get("start_offset", -1) or -1),
        int(site.get("ordinal", -1) or -1),
        str(site.get("id", "")),
    )


def resolve_site(
    manifest: Mapping[str, Any],
    selector: Mapping[str, Any],
    default_signature: str,
) -> Mapping[str, Any]:
    signature = str(selector.get("source_function_signature", default_signature))
    candidates: List[Mapping[str, Any]] = []
    for site in manifest.get("relay_sites", []):
        if not isinstance(site, Mapping) or not function_matches(site, signature):
            continue
        target = site.get("target", {})
        if not isinstance(target, Mapping):
            target = {}
        checks = {
            "target_text": str(target.get("text", "")),
            "target_function": str(site.get("target_function", "")),
            "relay_kind": str(site.get("relay_kind", "")),
            "target_scope": str(site.get("target_scope", "")),
            "source_scope": str(site.get("source_scope", "")),
        }
        if any(
            key in selector and str(selector[key]) != actual
            for key, actual in checks.items()
        ):
            continue
        if "argument_count" in selector and int(selector["argument_count"]) != len(
            site.get("arguments", [])
        ):
            continue
        candidates.append(site)
    candidates.sort(key=_site_sort_key)
    if "occurrence" in selector:
        occurrence = int(selector["occurrence"])
        if occurrence < 0 or occurrence >= len(candidates):
            raise BenchmarkError(
                f"site selector {dict(selector)} occurrence {occurrence} has "
                f"only {len(candidates)} candidates"
            )
        return candidates[occurrence]
    if len(candidates) != 1:
        raise BenchmarkError(
            f"site selector {dict(selector)} resolved to "
            f"{[str(site.get('id', '')) for site in candidates]}"
        )
    return candidates[0]


def resolve_sites(
    manifest: Mapping[str, Any], benchmark: Mapping[str, Any]
) -> Mapping[str, Mapping[str, Any]]:
    signature = str(benchmark["function_signature"])
    result: Dict[str, Mapping[str, Any]] = {}
    for label, selector in benchmark.get("sites", {}).items():
        result[str(label)] = resolve_site(manifest, selector, signature)
    ids = [str(site.get("id", "")) for site in result.values()]
    if len(ids) != len(set(ids)):
        raise BenchmarkError("semantic site labels do not resolve one-to-one")
    return result


def certificate_function(
    manifest: Mapping[str, Any], function_id: str
) -> Mapping[str, Any]:
    functions = manifest.get("parallel_certificate", {}).get("functions", [])
    matches = [
        entry
        for entry in functions
        if isinstance(entry, Mapping)
        and str(entry.get("source_function_id", "")) == function_id
    ]
    if len(matches) != 1:
        raise BenchmarkError(
            f"parallel certificate function {function_id!r} resolved to {len(matches)} rows"
        )
    # Keep the public five-argument ``evaluate_certificate`` helper useful
    # without weakening evidence validation: its certificate-function view
    # carries a private, read-only reference to the owning manifest.  The
    # reference is never serialized into a publication artifact.
    result = dict(matches[0])
    result["__manifest__"] = manifest
    return result


def evidence_ids(entry: Mapping[str, Any]) -> List[str]:
    result: List[str] = []
    certificate_id = str(entry.get("certificate_id", ""))
    if certificate_id:
        result.append(certificate_id)
    for field in (
        "supporting_cfg_fact_ids",
        "supporting_constraint_ids",
        "supporting_solver_result_ids",
    ):
        values = entry.get(field, [])
        if isinstance(values, Sequence) and not isinstance(values, (str, bytes)):
            result.extend(str(value) for value in values if str(value))
    return list(dict.fromkeys(result))


def _structured_ids(value: Any) -> set[str]:
    """Collect owning IDs from a schema-v5 tree without guessing aliases."""

    result: set[str] = set()
    if isinstance(value, Mapping):
        for key, child in value.items():
            if (key == "id" or str(key).endswith("_id")) and isinstance(
                child, str
            ) and child:
                result.add(child)
            result.update(_structured_ids(child))
    elif isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        for child in value:
            result.update(_structured_ids(child))
    return result


def _mapping_index(
    values: Any, *, id_field: str = "id"
) -> Mapping[str, Mapping[str, Any]]:
    if not isinstance(values, Sequence) or isinstance(values, (str, bytes)):
        return {}
    return {
        str(value.get(id_field, "")): value
        for value in values
        if isinstance(value, Mapping) and str(value.get(id_field, ""))
    }


def _same_site_pair(value: Mapping[str, Any], left: str, right: str) -> bool:
    return {
        str(value.get("site_a", "")),
        str(value.get("site_b", "")),
    } == {left, right}


def _obligation_matches_sites(
    obligation: Mapping[str, Any], function_id: str, left: str, right: str
) -> bool:
    if str(obligation.get("source_function_id", "")) != function_id:
        return False
    return {
        str(obligation.get("relay_site_id", "")),
        str(obligation.get("related_relay_site_id", "")),
    } == {left, right}


def _pair_evidence_gate(
    manifest: Mapping[str, Any],
    function_id: str,
    certificate_type: str,
    expected_relation: str,
    left: str,
    right: str,
    pair: Mapping[str, Any],
) -> Tuple[bool, str, Mapping[str, List[str]]]:
    """Validate the owning evidence needed for a proved pair certificate.

    Merely having non-empty evidence arrays is insufficient.  Each supported
    pair property has a distinct semantic proof shape, checked here against
    the referenced CFG nodes, refinement constraints, and solver obligations.
    """

    evidence = {
        "cfg_fact_ids": [
            str(value)
            for value in pair.get("supporting_cfg_fact_ids", [])
            if str(value)
        ],
        "constraint_ids": [
            str(value)
            for value in pair.get("supporting_constraint_ids", [])
            if str(value)
        ],
        "solver_result_ids": [
            str(value)
            for value in pair.get("supporting_solver_result_ids", [])
            if str(value)
        ],
    }
    constraints = _mapping_index(
        manifest.get("refinement", {}).get("constraints", [])
    )
    obligations = _mapping_index(
        manifest.get("refinement", {}).get("proof_obligations", [])
    )
    cfg_ids = _structured_ids(manifest.get("control_flow", {}))

    dangling_cfg = [value for value in evidence["cfg_fact_ids"] if value not in cfg_ids]
    dangling_constraints = [
        value for value in evidence["constraint_ids"] if value not in constraints
    ]
    dangling_solver = [
        value for value in evidence["solver_result_ids"] if value not in obligations
    ]
    if dangling_cfg or dangling_constraints or dangling_solver:
        return (
            False,
            "certificate evidence contains dangling IDs: "
            f"cfg={dangling_cfg}, constraints={dangling_constraints}, "
            f"solver={dangling_solver}",
            evidence,
        )

    relation = str(pair.get("relation", ""))
    if relation != expected_relation:
        return (
            False,
            f"certificate relation {relation!r} does not match {expected_relation!r}",
            evidence,
        )
    if str(pair.get("status", "")) != "Proved":
        return False, "pair certificate status is not Proved", evidence

    referenced_obligations = [
        obligations[value] for value in evidence["solver_result_ids"]
    ]

    if certificate_type in {"MutualExclusive", "MutuallyExclusive"}:
        proved = [
            value
            for value in referenced_obligations
            if str(value.get("kind", "")) == "RelayMutualExclusion"
            and _obligation_matches_sites(value, function_id, left, right)
            and str(value.get("solver_result", {}).get("status", "")) == "Proved"
        ]
        if not proved:
            return (
                False,
                "MutuallyExclusive lacks a matching Proved RelayMutualExclusion obligation",
                evidence,
            )
        return True, "", evidence

    if certificate_type == "CoEmissionIndependent":
        target_sites = {
            str(constraints[value].get("relay_site_id", ""))
            for value in evidence["constraint_ids"]
            if str(constraints[value].get("kind", ""))
            == "RelayTargetRelation"
            and str(constraints[value].get("source_function_id", ""))
            == function_id
        }
        if not {left, right}.issubset(target_sites):
            return (
                False,
                "CoEmissionIndependent lacks target-relation constraints for both relay sites",
                evidence,
            )
        co_emission = [
            value
            for value in referenced_obligations
            if str(value.get("kind", "")) == "RelayMutualExclusion"
            and _obligation_matches_sites(value, function_id, left, right)
            and str(value.get("solver_result", {}).get("status", ""))
            == "Disproved"
        ]
        independence = [
            value
            for value in referenced_obligations
            if str(value.get("kind", "")) == "RelayTargetIndependence"
            and _obligation_matches_sites(value, function_id, left, right)
            and str(value.get("solver_result", {}).get("status", "")) == "Proved"
        ]
        if not co_emission or not independence:
            return (
                False,
                "CoEmissionIndependent requires co-emission feasibility "
                "(RelayMutualExclusion Disproved) and target independence Proved",
                evidence,
            )
        return True, "", evidence

    if certificate_type == "MustPrecede":
        if expected_relation not in {"MustPrecedeAB", "MustPrecedeBA"}:
            return False, "MustPrecede expectation has no proved direction", evidence
        semantic_before, semantic_after = (
            (str(pair.get("site_a", "")), str(pair.get("site_b", "")))
            if expected_relation == "MustPrecedeAB"
            else (str(pair.get("site_b", "")), str(pair.get("site_a", "")))
        )
        if (semantic_before, semantic_after) != (left, right):
            return False, "MustPrecede relation direction does not match site order", evidence

        relay_emit_sites: set[str] = set()
        functions = manifest.get("control_flow", {}).get("functions", [])
        if isinstance(functions, Sequence) and not isinstance(functions, (str, bytes)):
            for function in functions:
                if not isinstance(function, Mapping):
                    continue
                for node in function.get("nodes", []):
                    if (
                        isinstance(node, Mapping)
                        and str(node.get("id", "")) in evidence["cfg_fact_ids"]
                        and str(node.get("kind", "")) == "RelayEmit"
                    ):
                        relay_emit_sites.add(str(node.get("relay_site_id", "")))
        if not {left, right}.issubset(relay_emit_sites):
            return (
                False,
                "MustPrecede lacks RelayEmit CFG facts for both relay sites",
                evidence,
            )
        return True, "", evidence

    return False, f"unsupported pair certificate type {certificate_type!r}", evidence


def finite_upper_bound(entry: Mapping[str, Any]) -> int | None:
    upper = entry.get("upper_bound", {})
    if isinstance(upper, Mapping) and str(upper.get("kind", "")).lower() == "constant":
        try:
            return int(upper.get("value"))
        except (TypeError, ValueError):
            return None
    if str(entry.get("bound_kind", "")).lower() == "constant":
        try:
            coefficient = int(entry.get("active_shard_count_coefficient", 0) or 0)
            return int(entry.get("constant_term")) if coefficient == 0 else None
        except (TypeError, ValueError):
            return None
    return None


def is_finite_certificate_bound(entry: Mapping[str, Any], field: str) -> bool:
    """Return whether ``entry`` owns a finite, established upper bound."""

    if str(entry.get("status", "")) not in {"Complete", "Conservative", "Proved"}:
        return False
    if field == "physical_route_work":
        if str(entry.get("bound_kind", "")).lower() != "constant":
            return False
        try:
            return (
                int(entry.get("active_shard_count_coefficient", 0) or 0) == 0
                and int(entry.get("constant_term")) >= 0
            )
        except (TypeError, ValueError):
            return False
    return finite_upper_bound(entry) is not None


def evaluate_certificate(
    benchmark: Mapping[str, Any],
    expectation: Mapping[str, Any],
    function_id: str,
    sites: Mapping[str, Mapping[str, Any]],
    function_certificate: Mapping[str, Any],
) -> MutableMapping[str, Any]:
    certificate_type = str(expectation["certificate_type"])
    record: MutableMapping[str, Any] = {
        "record_id": str(expectation["id"]),
        "workload": str(benchmark["name"]),
        "pattern": str(expectation.get("pattern", "")),
        "certificate_type": certificate_type,
        "source_function_signature": str(benchmark["function_signature"]),
        "source_function_id": function_id,
        "relay_site_a": "",
        "relay_site_b": "",
        "status": "Missing",
        "raw_certificate_status": "Missing",
        "validation_status": "Failed",
        "evidence_ids": [],
        "reason": "",
        "mutation_mapping": dict(expectation.get("mutation", {})),
        "mutation": {"status": "NotRun"},
    }
    if certificate_type in PAIR_CERTIFICATE_TYPES:
        left = str(sites[str(expectation["site_a"])].get("id", ""))
        right = str(sites[str(expectation["site_b"])].get("id", ""))
        relation = str(expectation["expected_relation"])
        record["relay_site_a"] = left
        record["relay_site_b"] = right
        matches = [
            pair
            for pair in function_certificate.get("pair_relations", [])
            if isinstance(pair, Mapping)
            and str(pair.get("site_a", "")) == left
            and str(pair.get("site_b", "")) == right
            and str(pair.get("relation", "")) == relation
        ]
        if len(matches) != 1:
            record["reason"] = (
                f"expected exactly one {relation} relation for {left},{right}; "
                f"found {len(matches)}"
            )
            return record
        pair = matches[0]
        raw_status = str(pair.get("status", ""))
        expected_status = str(expectation.get("expected_status", "Proved"))
        manifest = function_certificate.get("__manifest__")
        if not isinstance(manifest, Mapping):
            evidence_valid = False
            evidence_reason = "owning manifest is unavailable for evidence validation"
            structured_evidence = {
                "cfg_fact_ids": [],
                "constraint_ids": [],
                "solver_result_ids": [],
            }
        else:
            evidence_valid, evidence_reason, structured_evidence = _pair_evidence_gate(
                manifest,
                function_id,
                certificate_type,
                relation,
                left,
                right,
                pair,
            )
        passed = raw_status == expected_status and evidence_valid
        record.update(
            {
                "status": raw_status if passed else "Unsupported",
                "raw_certificate_status": raw_status,
                "validation_status": "Passed" if passed else "Failed",
                "evidence_ids": evidence_ids(pair),
                "reason": (
                    str(pair.get("reason", ""))
                    if passed
                    else evidence_reason
                    or f"certificate status {raw_status!r} does not match {expected_status!r}"
                ),
                "expected_relation": relation,
                "relation": str(pair.get("relation", "")),
                "certificate_id": str(pair.get("certificate_id", "")),
                "evidence_valid": evidence_valid,
                "evidence": structured_evidence,
            }
        )
        return record

    field = str(expectation["bound_field"])
    bound = function_certificate.get(field)
    if not isinstance(bound, Mapping):
        record["reason"] = f"bound field {field!r} is absent"
        return record
    observed = finite_upper_bound(bound)
    expected = int(expectation["expected_upper_bound"])
    raw_status = str(bound.get("status", ""))
    passed = observed == expected and is_finite_certificate_bound(bound, field)
    # A conservative exact-count status still carries a proved finite upper
    # bound.  Report both views rather than relabeling the compiler field.
    property_status = "Proved" if passed else raw_status or "Missing"
    record.update(
        {
            "status": property_status,
            "raw_certificate_status": raw_status,
            "validation_status": "Passed" if passed else "Failed",
            "evidence_ids": evidence_ids(bound),
            "reason": str(bound.get("reason", "")),
            "certificate_id": str(bound.get("certificate_id", "")),
            "evidence_valid": bool(evidence_ids(bound)),
            "bound_field": field,
            "observed_upper_bound": observed,
            "expected_upper_bound": expected,
        }
    )
    if passed and not record["evidence_ids"]:
        record["status"] = "Unsupported"
        record["validation_status"] = "Failed"
        record["evidence_valid"] = False
        record["reason"] = "finite bound has no evidence IDs"
    return record


def resolve_relay_site(
    manifest: Mapping[str, Any], selector: Mapping[str, Any]
) -> Mapping[str, Any]:
    """Public semantic relay-site locator used by evidence-only tests."""

    signature = str(selector.get("source_function_signature", ""))
    if not signature:
        raise BenchmarkError("relay-site selector requires source_function_signature")
    return resolve_site(manifest, selector, signature)


def evaluate_certificate_expectation(
    manifest: Mapping[str, Any], expectation: Mapping[str, Any]
) -> MutableMapping[str, Any]:
    """Evaluate a direct-ID certificate expectation with the same strict gate.

    The executable benchmark configuration uses semantic labels and calls
    :func:`evaluate_certificate` directly.  This adapter is intentionally
    narrow for tools that already resolved stable IDs from a fresh manifest.
    """

    function_id = str(expectation.get("source_function_id", ""))
    left = str(expectation.get("relay_site_a", ""))
    right = str(expectation.get("relay_site_b", ""))
    sites_by_id = _mapping_index(manifest.get("relay_sites", []))
    if not function_id or left not in sites_by_id or right not in sites_by_id:
        raise BenchmarkError("direct certificate expectation has unresolved IDs")
    certificate_type = str(expectation.get("certificate_type", ""))
    relation = str(expectation.get("expected_relation", ""))
    if not relation:
        if certificate_type in {"MutualExclusive", "MutuallyExclusive"}:
            relation = "MutuallyExclusive"
        elif certificate_type == "CoEmissionIndependent":
            relation = "CoEmissionIndependent"
        elif certificate_type == "MustPrecede":
            before = str(expectation.get("before_site_id", left))
            after = str(expectation.get("after_site_id", right))
            if (before, after) == (left, right):
                relation = "MustPrecedeAB"
            elif (before, after) == (right, left):
                relation = "MustPrecedeBA"
            else:
                relation = ""
    translated = {
        "id": str(expectation.get("id", "direct.expectation")),
        "pattern": str(expectation.get("pattern", "")),
        "certificate_type": certificate_type,
        "site_a": "left",
        "site_b": "right",
        "expected_relation": relation,
        "expected_status": str(expectation.get("expected_status", "Proved")),
        "mutation": dict(expectation.get("mutation", {})),
    }
    signature = str(sites_by_id[left].get("source_function_signature", function_id))
    return evaluate_certificate(
        {
            "name": str(expectation.get("workload", "DirectExpectation")),
            "function_signature": signature,
        },
        translated,
        function_id,
        {"left": sites_by_id[left], "right": sites_by_id[right]},
        certificate_function(manifest, function_id),
    )


def baseline_runtime_requirements(
    benchmark: Mapping[str, Any],
    function_id: str,
    sites: Mapping[str, Mapping[str, Any]],
) -> Mapping[str, Any]:
    config = benchmark.get("runtime_coverage", {})
    if not isinstance(config, Mapping):
        config = {}
    labels = config.get("required_semantic_sites", list(sites.keys()))
    required_sites: List[str] = []
    for label in labels:
        if str(label) not in sites:
            raise BenchmarkError(f"runtime coverage references unknown site {label!r}")
        required_sites.append(str(sites[str(label)].get("id", "")))
    signatures = config.get(
        "required_function_signatures", [benchmark["function_signature"]]
    )
    required_functions = []
    for signature in signatures:
        if str(signature) != str(benchmark["function_signature"]):
            raise BenchmarkError(
                "this runner currently resolves runtime functions only within "
                "the benchmark source function"
            )
        required_functions.append(function_id)
    unresolved = [str(value) for value in config.get("unresolved_site_ids", [])]
    return {
        "label": str(benchmark["name"]),
        "required_function_ids": sorted(set(required_functions)),
        "required_site_ids": sorted(set(required_sites)),
        "unresolved_site_ids": sorted(set(unresolved)),
    }


def mutation_kinds(benchmark: Mapping[str, Any]) -> List[str]:
    result = {
        str(entry.get("mutation", {}).get("kind", ""))
        for entry in benchmark.get("certificates", [])
        if isinstance(entry, Mapping)
        and isinstance(entry.get("mutation"), Mapping)
        and entry.get("mutation", {}).get("validation_required") is True
    }
    unknown = result - SUPPORTED_MUTATIONS
    if unknown:
        raise BenchmarkError(f"unsupported required mutation kinds: {sorted(unknown)}")
    return sorted(result)


def select_mutation(
    index: Mapping[str, Any],
    expectation: Mapping[str, Any],
    function_id: str,
    sites: Mapping[str, Mapping[str, Any]],
) -> Mapping[str, Any]:
    mutation = expectation.get("mutation", {})
    kind = str(mutation.get("kind", ""))
    affected = [
        str(sites[str(label)].get("id", "")) for label in mutation.get("sites", [])
    ]
    matches = [
        record
        for record in index.get("mutations", [])
        if isinstance(record, Mapping)
        and record.get("generation_status") == "Generated"
        and str(record.get("mutation_type", "")) == kind
        and str(record.get("source_function_id", "")) == function_id
        and [str(value) for value in record.get("relay_site_ids", [])] == affected
    ]
    if len(matches) != 1:
        raise BenchmarkError(
            f"{expectation['id']}: mutation selector kind={kind}, sites={affected} "
            f"resolved to {len(matches)} candidates"
        )
    return matches[0]


def expected_regression_matches(
    regressions: Sequence[str], mutation: Mapping[str, Any]
) -> bool:
    expected = mutation.get("expected_regression_contains", "")
    fragments = (
        [str(value) for value in expected]
        if isinstance(expected, Sequence) and not isinstance(expected, (str, bytes))
        else [str(expected)]
    )
    fragments = [value for value in fragments if value]
    return bool(fragments) and all(
        any(fragment in regression for regression in regressions)
        for fragment in fragments
    )


def validate_mutation_regression(
    certificate_type: str,
    mutation: Mapping[str, Any],
    baseline: Mapping[str, Any],
    mutant_records: Sequence[Mapping[str, Any]],
) -> Mapping[str, Any]:
    """Validate a certificate-to-operator mapping by semantic regression."""

    expected_operator = {
        "MutualExclusive": "GuardNegate",
        "MutuallyExclusive": "GuardNegate",
        "MustPrecede": "RelayOrderSwap",
        "CoEmissionIndependent": "IntroduceAlias",
        "WorkBound": "RelayDuplicate",
    }.get(certificate_type, "")
    actual_operator = str(
        mutation.get("kind", mutation.get("mutation_type", ""))
    )
    result: Dict[str, Any] = {
        "valid": False,
        "status": "NotDetected",
        "mutation_id": str(mutation.get("mutation_id", "")),
        "mutation_type": actual_operator,
        "reason": "",
    }
    if not expected_operator or actual_operator != expected_operator:
        result["reason"] = (
            f"{certificate_type} requires mutation operator {expected_operator!r}, "
            f"got {actual_operator!r}"
        )
        return result
    if str(baseline.get("status", "")) != "Proved":
        result["reason"] = "baseline certificate is not Proved"
        return result

    left = str(baseline.get("relay_site_a", ""))
    right = str(baseline.get("relay_site_b", ""))
    candidates = []
    for value in mutant_records:
        if str(value.get("certificate_type", "")) != certificate_type:
            continue
        mutant_left = str(value.get("relay_site_a", ""))
        mutant_right = str(value.get("relay_site_b", ""))
        if left or right:
            if {mutant_left, mutant_right} != {left, right}:
                continue
        candidates.append(value)
    if len(candidates) != 1:
        result["reason"] = f"semantic mutant certificate resolved to {len(candidates)} rows"
        return result

    mutant = candidates[0]
    baseline_relation = str(baseline.get("relation", ""))
    mutant_relation = str(mutant.get("relation", ""))
    regressed = str(mutant.get("status", "")) != "Proved"
    if baseline_relation:
        regressed = regressed or mutant_relation != baseline_relation
    if certificate_type == "WorkBound":
        try:
            baseline_bound = int(baseline.get("observed_upper_bound"))
            mutant_bound = int(mutant.get("observed_upper_bound"))
            regressed = regressed or mutant_bound > baseline_bound
        except (TypeError, ValueError):
            pass
    if not regressed:
        result["reason"] = "mutant preserves the baseline certificate"
        return result
    result.update(
        {
            "valid": True,
            "status": "Detected",
            "reason": "the mapped mutation causes a semantic certificate regression",
            "mutant_status": str(mutant.get("status", "")),
            "mutant_relation": mutant_relation,
        }
    )
    return result


def portable_record(record: Mapping[str, Any], mr: Any, output: pathlib.Path) -> Dict[str, Any]:
    result = json.loads(json.dumps(record))
    for field in ("original_code_path", "mutated_code_path"):
        if result.get(field):
            result[field] = mr.portable_path(str(result[field]), output)
    return result


def publish_generation_index(
    index: Mapping[str, Any],
    index_path: pathlib.Path,
    mr: Any,
    output: pathlib.Path,
) -> None:
    public = json.loads(json.dumps(index))
    for field in ("source_path", "manifest_path"):
        if public.get(field):
            public[field] = mr.portable_path(str(public[field]), output)
    for record in public.get("mutations", []):
        for field in ("original_code_path", "mutated_code_path"):
            if record.get(field):
                record[field] = mr.portable_path(str(record[field]), output)
        mutation_id = str(record.get("mutation_id", ""))
        metadata = index_path.parent / "mutants" / mutation_id / "mutation.json"
        if mutation_id and metadata.is_file():
            write_json(metadata, record)
    write_json(index_path, public)


def redactor(
    repo_root: pathlib.Path,
    output: pathlib.Path,
    config: pathlib.Path,
    external_paths: Iterable[pathlib.Path],
):
    replacements: List[Tuple[str, str]] = [
        (str(output.resolve()), "<output>"),
        (str(repo_root.resolve()), "<repo>"),
        (str(pathlib.Path.home().resolve()), "<home>"),
        (str(config.resolve()), "<config>"),
    ]
    for path in external_paths:
        resolved = path.resolve()
        try:
            resolved.relative_to(repo_root.resolve())
            continue
        except ValueError:
            pass
        try:
            resolved.relative_to(output.resolve())
            continue
        except ValueError:
            pass
        replacements.append((str(resolved), f"<external>/{resolved.name}"))
    replacements = sorted(set(replacements), key=lambda item: len(item[0]), reverse=True)

    def sanitize(value: Any) -> Any:
        if isinstance(value, Mapping):
            return {str(key): sanitize(item) for key, item in value.items()}
        if isinstance(value, list):
            return [sanitize(item) for item in value]
        if isinstance(value, tuple):
            return [sanitize(item) for item in value]
        if isinstance(value, str):
            for original, replacement in replacements:
                value = value.replace(original, replacement)
            return value
        return value

    return sanitize


def write_csv(path: pathlib.Path, records: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=list(CSV_FIELDS),
            lineterminator="\n",
        )
        writer.writeheader()
        for record in records:
            mutation = record.get("mutation", {})
            if not isinstance(mutation, Mapping):
                mutation = {}
            row = {
                "record_id": record.get("record_id", ""),
                "workload": record.get("workload", ""),
                "pattern": record.get("pattern", ""),
                "certificate_type": record.get("certificate_type", ""),
                "source_function_signature": record.get(
                    "source_function_signature", ""
                ),
                "relay_site_a": record.get("relay_site_a", ""),
                "relay_site_b": record.get("relay_site_b", ""),
                "status": record.get("status", ""),
                "raw_certificate_status": record.get(
                    "raw_certificate_status", ""
                ),
                "validation_status": record.get("validation_status", ""),
                "evidence_ids": canonical_json(list(record.get("evidence_ids", []))),
                "reason": record.get("reason", ""),
                "mutation_mapping": canonical_json(
                    record.get("mutation_mapping", {})
                ),
                "mutation_id": mutation.get("mutation_id", ""),
                "mutation_type": mutation.get("mutation_type", ""),
                "mutation_generation_status": mutation.get(
                    "generation_status", ""
                ),
                "mutation_compile_status": mutation.get("compile_status", ""),
                "mutation_runtime_status": mutation.get("runtime_status", ""),
                "mutation_detected": mutation.get("detected", ""),
                "mutation_regressions": ";".join(
                    mutation.get("regressions", [])
                ),
            }
            writer.writerow(row)
    temporary.replace(path)


def write_extension_results(output: pathlib.Path, payload: Mapping[str, Any]) -> None:
    """Validate and atomically publish the isolated JSON/CSV products."""

    records = payload.get("records", [])
    if not isinstance(records, Sequence) or isinstance(records, (str, bytes)):
        raise BenchmarkError("extension result records must be an array")
    required = {
        "workload",
        "pattern",
        "certificate_type",
        "relay_site_a",
        "relay_site_b",
        "status",
        "evidence_ids",
        "mutation_mapping",
    }
    for index, record in enumerate(records):
        if not isinstance(record, Mapping):
            raise BenchmarkError(f"extension result record {index} is not an object")
        missing = sorted(required - set(record))
        if missing:
            raise BenchmarkError(
                f"extension result record {index} lacks fields: {missing}"
            )
        if not isinstance(record.get("evidence_ids"), Sequence) or isinstance(
            record.get("evidence_ids"), (str, bytes)
        ):
            raise BenchmarkError(f"extension result record {index} evidence_ids is not an array")
        if not isinstance(record.get("mutation_mapping"), Mapping):
            raise BenchmarkError(
                f"extension result record {index} mutation_mapping is not an object"
            )

    serialized = json.dumps(payload, ensure_ascii=False)
    if re.search(r"/(?:home|Users)/", serialized):
        raise BenchmarkError("publication payload contains a private home path")
    write_json(output / "certificate_extension.json", payload)
    write_csv(output / "certificate_extension.csv", records)


def git_value(repo_root: pathlib.Path, arguments: Sequence[str]) -> str:
    try:
        result = subprocess.run(
            ["git", *arguments],
            cwd=str(repo_root),
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def run_benchmark(
    benchmark: Mapping[str, Any],
    defaults: Mapping[str, Any],
    repo_root: pathlib.Path,
    config_directory: pathlib.Path,
    output: pathlib.Path,
    engine: pathlib.Path,
    chsimu: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
    seed: int,
    timeout: float,
    mr: Any,
) -> Tuple[Mapping[str, Any], List[MutableMapping[str, Any]], List[pathlib.Path]]:
    name = str(benchmark["name"])
    work = output / "work" / slug(name)
    source = resolve_repo_path(
        str(benchmark["source"]), repo_root, config_directory
    )
    template = resolve_repo_path(
        str(benchmark["runtime_template"]),
        repo_root,
        config_directory,
        config_first=True,
    )
    if not source.is_file() or not template.is_file():
        raise BenchmarkError(f"{name}: source or runtime template is missing")
    if "{{SOURCE}}" not in template.read_text(encoding="utf-8"):
        raise BenchmarkError(f"{name}: runtime template lacks {{{{SOURCE}}}}")

    compile_result = mr.compile_source(
        source,
        work / "baseline" / "compile",
        chsimu,
        library_paths,
        path_prefixes,
        timeout,
        output,
    )
    if compile_result.status != "Compiled" or compile_result.manifest is None:
        raise BenchmarkError(
            f"{name}: fresh baseline compile failed: "
            f"{compile_result.status}: {compile_result.reason}"
        )
    manifest = compile_result.manifest
    manifest_validation = validate_manifest(manifest)
    oracle = work / "baseline" / "oracle.relay_protocol.json"
    write_json(oracle, manifest)
    function_id = resolve_function_id(manifest, str(benchmark["function_signature"]))
    sites = resolve_sites(manifest, benchmark)
    function_certificate = certificate_function(manifest, function_id)
    records = [
        evaluate_certificate(
            benchmark, entry, function_id, sites, function_certificate
        )
        for entry in benchmark.get("certificates", [])
    ]

    runtime_arguments = [
        str(value)
        for value in benchmark.get(
            "runtime_arguments",
            [
                f"-seed:{seed}",
                f"-order:{int(defaults.get('order', 2))}",
                f"-addresses:{int(defaults.get('addresses', 8))}",
            ],
        )
    ]
    coverage = baseline_runtime_requirements(
        benchmark, function_id, sites
    )
    runtime = mr.run_runtime_strict(
        source,
        template,
        work / "baseline" / "runtime",
        chsimu,
        library_paths,
        path_prefixes,
        timeout,
        runtime_arguments,
        coverage,
        output,
    )
    baseline_runtime_passed = runtime.get("status") == "Passed" and bool(
        runtime.get("coverage", {}).get("covered", False)
    )

    kinds = mutation_kinds(benchmark)
    index: Mapping[str, Any] = {
        "diagnostics": [],
        "operator_counts": {},
        "mutations": [],
    }
    generation_process_summary: Mapping[str, Any] = {"status": "NotRun"}
    if kinds:
        generation_directory = work / "generation"
        command = [
            str(engine),
            "--source",
            str(source),
            "--manifest",
            str(oracle),
            "--output",
            str(generation_directory),
            "--seed",
            str(seed),
            "--max-per-kind",
            "0",
            "--kinds",
            ",".join(kinds),
            "--include-inapplicable",
        ]
        process = mr.run_process(
            command,
            repo_root,
            mr.process_environment(chsimu, library_paths, path_prefixes),
            timeout,
        )
        mr.persist_process(work / "generation_process", process, output)
        index_path = generation_directory / "mutation_index.json"
        generation_process_summary = {
            "returncode": process.returncode,
            "timed_out": process.timed_out,
            "elapsed_seconds": process.elapsed_seconds,
            "launch_error": process.launch_error,
        }
        if (
            process.launch_error
            or process.timed_out
            or process.returncode != 0
            or not index_path.is_file()
        ):
            raise BenchmarkError(f"{name}: mutation generator failed")
        index = load_json(index_path)
        diagnostics = index.get("diagnostics", [])
        if diagnostics:
            raise BenchmarkError(
                f"{name}: mutation generator diagnostics: {diagnostics}"
            )
        raw_index = index
        # Sanitize disk metadata immediately.  Keep the live absolute paths in
        # raw_index for compilation and source-aware site alignment.
        publish_generation_index(raw_index, index_path, mr, output)

        compiled_cache: Dict[str, Tuple[Mapping[str, Any], Mapping[str, Any]]] = {}
        for expectation, record in zip(benchmark.get("certificates", []), records):
            mutation_config = expectation.get("mutation", {})
            if not isinstance(mutation_config, Mapping) or not mutation_config.get(
                "validation_required", False
            ):
                record["mutation"] = {
                    "status": "NotRunPositiveOnly",
                    "mutation_type": str(mutation_config.get("kind", "PositiveOnly")),
                    "reason": str(mutation_config.get("reason", "")),
                    "detected": None,
                }
                continue
            generated = select_mutation(raw_index, expectation, function_id, sites)
            mutation_id = str(generated.get("mutation_id", ""))
            if mutation_id not in compiled_cache:
                mutant_source = pathlib.Path(
                    str(generated.get("mutated_code_path", ""))
                )
                if not mutant_source.is_file():
                    raise BenchmarkError(
                        f"{name}: generated mutant source is missing: {mutation_id}"
                    )
                mutant_run = work / "mutants" / mutation_id
                mutant_compile = mr.compile_source(
                    mutant_source,
                    mutant_run / "compile",
                    chsimu,
                    library_paths,
                    path_prefixes,
                    timeout,
                    output,
                )
                if (
                    mutant_compile.status != "Compiled"
                    or mutant_compile.manifest is None
                ):
                    raise BenchmarkError(
                        f"{name}: mutant {mutation_id} did not compile: "
                        f"{mutant_compile.status}: {mutant_compile.reason}"
                    )
                validate_manifest(mutant_compile.manifest)
                mutant_coverage = mr.mutation_coverage_requirements(
                    generated, manifest, mutant_compile.manifest
                )
                mutant_runtime = mr.run_runtime_strict(
                    mutant_source,
                    template,
                    mutant_run / "runtime",
                    chsimu,
                    library_paths,
                    path_prefixes,
                    timeout,
                    runtime_arguments,
                    mutant_coverage,
                    output,
                )
                compiled_cache[mutation_id] = (
                    {
                        "compile_status": mutant_compile.status,
                        "manifest": mutant_compile.manifest,
                        "manifest_digest": digest_json(mutant_compile.manifest),
                        "runtime": mutant_runtime,
                    },
                    generated,
                )
            compiled, generated = compiled_cache[mutation_id]
            regressions = mr.certificate_regressions(
                manifest, compiled["manifest"], generated
            )
            matched = expected_regression_matches(regressions, mutation_config)
            mutant_runtime = compiled["runtime"]
            runtime_passed = mutant_runtime.get("status") == "Passed" and bool(
                mutant_runtime.get("coverage", {}).get("covered", False)
            )
            detected = bool(matched and runtime_passed)
            record["mutation"] = {
                **portable_record(generated, mr, output),
                "status": "Detected" if detected else "Failed",
                "compile_status": compiled["compile_status"],
                "manifest_digest": compiled["manifest_digest"],
                "runtime_status": mutant_runtime.get("status", "NotRun"),
                "runtime_coverage": mutant_runtime.get("coverage", {}),
                "runtime_skipped_unsupported": mutant_runtime.get(
                    "skipped_unsupported", 0
                ),
                "regressions": regressions,
                "expected_regression_matched": matched,
                "detected": detected,
            }
    else:
        raw_index = index

    records_passed = all(
        record.get("validation_status") == "Passed"
        and (
            not record.get("mutation_mapping", {}).get("validation_required", False)
            or record.get("mutation", {}).get("detected") is True
        )
        for record in records
    )
    status = "Passed" if baseline_runtime_passed and records_passed else "Failed"
    workload_result = {
        "workload": name,
        "status": status,
        "source": mr.portable_path(source, output),
        "source_sha256": sha256_file(source),
        "runtime_template": mr.portable_path(template, output),
        "runtime_template_sha256": sha256_file(template),
        "function_signature": str(benchmark["function_signature"]),
        "function_id": function_id,
        "manifest_validation": manifest_validation,
        "baseline_manifest_digest": digest_json(manifest),
        "resolved_sites": {
            label: {
                "relay_site_id": str(site.get("id", "")),
                "target_text": str(site.get("target", {}).get("text", "")),
                "target_function": str(site.get("target_function", "")),
                "relay_kind": str(site.get("relay_kind", "")),
                "target_scope": str(site.get("target_scope", "")),
                "location": site.get("location", {}),
            }
            for label, site in sites.items()
        },
        "baseline": {
            "compile_status": compile_result.status,
            "runtime_status": runtime.get("status", "NotRun"),
            "runtime_coverage": runtime.get("coverage", {}),
            "runtime_passed_checks": runtime.get("passed", 0),
            "runtime_skipped_unsupported": runtime.get("skipped_unsupported", 0),
            "runtime_infrastructure_failures": runtime.get(
                "infrastructure_failures", []
            ),
        },
        "mutation_generation": {
            **generation_process_summary,
            "kinds": kinds,
            "operator_counts": index.get("operator_counts", {}),
            "diagnostics": index.get("diagnostics", []),
        },
        "certificate_record_ids": [record["record_id"] for record in records],
    }
    return workload_result, records, [source, template, engine, chsimu]


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, type=pathlib.Path)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=detect_repo_root(),
    )
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--engine", type=pathlib.Path)
    parser.add_argument("--chsimu", type=pathlib.Path)
    parser.add_argument("--library-path", action="append", default=[])
    parser.add_argument("--path-prefix", action="append", default=[])
    parser.add_argument("--seed", type=int)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--validate-config-only", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    config_path = args.config.expanduser().resolve()
    repo_root = args.repo_root.expanduser().resolve()
    config = load_json(config_path)
    validate_configuration(config)
    defaults = config.get("defaults", {})
    output_subdirectory = str(
        config.get("output_subdirectory", "certificate_extension")
    )
    output = (
        args.output.expanduser().resolve()
        if args.output is not None
        else (repo_root / "results" / output_subdirectory).resolve()
    )
    engine = (
        args.engine.expanduser().resolve()
        if args.engine is not None
        else repo_root / "bin" / "bin_release" / "rpreda_mutation"
    )
    chsimu = (
        args.chsimu.expanduser().resolve()
        if args.chsimu is not None
        else repo_root / "bin" / "bin_release" / "chsimu"
    )
    for label, path in (
        ("repository", repo_root),
        ("mutation engine", engine),
        ("chsimu", chsimu),
    ):
        if not path.exists():
            raise BenchmarkError(f"{label} does not exist: {path}")
    if args.validate_config_only:
        print(f"Configuration valid: {len(config['benchmarks'])} workloads")
        return 0

    seed = int(args.seed if args.seed is not None else defaults.get("seed", 88))
    timeout = float(
        args.timeout
        if args.timeout is not None
        else defaults.get("timeout_seconds", 60)
    )
    if timeout <= 0:
        raise BenchmarkError("timeout must be positive")
    output.mkdir(parents=True, exist_ok=True)
    mr = load_mutation_runner(repo_root)
    started = utc_now()
    workloads: List[Mapping[str, Any]] = []
    records: List[MutableMapping[str, Any]] = []
    external_paths: List[pathlib.Path] = [config_path, engine, chsimu]
    failures: List[Mapping[str, str]] = []
    for benchmark in config["benchmarks"]:
        name = str(benchmark["name"])
        print(f"[{len(workloads) + len(failures) + 1}/{len(config['benchmarks'])}] {name}", flush=True)
        try:
            workload, rows, paths = run_benchmark(
                benchmark,
                defaults,
                repo_root,
                config_path.parent,
                output,
                engine,
                chsimu,
                [str(pathlib.Path(value).expanduser().resolve()) for value in args.library_path],
                [str(pathlib.Path(value).expanduser().resolve()) for value in args.path_prefix],
                seed,
                timeout,
                mr,
            )
            workloads.append(workload)
            records.extend(rows)
            external_paths.extend(paths)
        except Exception as exc:
            failures.append({"workload": name, "reason": str(exc)})
            workloads.append({"workload": name, "status": "Failed", "reason": str(exc)})

    counts = Counter(str(record.get("certificate_type", "")) for record in records)
    status_counts = Counter(str(record.get("status", "")) for record in records)
    passed_records = sum(
        record.get("validation_status") == "Passed"
        and (
            not record.get("mutation_mapping", {}).get("validation_required", False)
            or record.get("mutation", {}).get("detected") is True
        )
        for record in records
    )
    payload: Mapping[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "study": str(config.get("study", "parallel_certificate_extension")),
        "metadata": {
            "started_at": started,
            "completed_at": utc_now(),
            "seed": seed,
            "timeout_seconds": timeout,
            "config": "<config>",
            "config_sha256": sha256_file(config_path),
            "repo_commit": git_value(repo_root, ["rev-parse", "HEAD"]),
            "repo_dirty_tracked_files": git_value(
                repo_root, ["status", "--short", "--untracked-files=no"]
            ).splitlines(),
            "mutation_engine": mr.portable_path(engine, output),
            "mutation_engine_sha256": sha256_file(engine),
            "chsimu": mr.portable_path(chsimu, output),
            "chsimu_sha256": sha256_file(chsimu),
            "aggregate_with_original_coverage": False,
        },
        "summary": {
            "status": "Passed"
            if not failures
            and len(workloads) == len(config["benchmarks"])
            and all(workload.get("status") == "Passed" for workload in workloads)
            else "Failed",
            "configured_workloads": len(config["benchmarks"]),
            "completed_workloads": sum(
                workload.get("status") == "Passed" for workload in workloads
            ),
            "certificate_records": len(records),
            "passed_certificate_records": passed_records,
            "certificate_type_counts": dict(sorted(counts.items())),
            "status_counts": dict(sorted(status_counts.items())),
            "failures": failures,
        },
        "workloads": workloads,
        "records": records,
    }
    sanitize = redactor(repo_root, output, config_path, external_paths)
    public_payload = sanitize(payload)
    public_records = sanitize(records)
    # Publish only through the schema/path-leak gate.  Keep the separately
    # sanitized records assignment above as a defensive consistency check.
    if public_payload.get("records") != public_records:
        raise BenchmarkError("sanitized record projection is inconsistent")
    write_extension_results(output, public_payload)
    # The two publication products must never expose an absolute home path.
    for path in (
        output / "certificate_extension.json",
        output / "certificate_extension.csv",
    ):
        text = path.read_text(encoding="utf-8")
        if re.search(r"/(?:home|Users)/[^\s\"',;]+", text):
            raise BenchmarkError(f"publication path leak detected in {path.name}")
    print(
        f"Parallel certificate extension: {payload['summary']['status']}; "
        f"results={output / 'certificate_extension.json'}"
    )
    return 0 if payload["summary"]["status"] == "Passed" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BenchmarkError as exc:
        print(f"ParallelCertificateBenchmarkRunner: {exc}", file=sys.stderr)
        raise SystemExit(2)
