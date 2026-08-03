#!/usr/bin/env python3
"""Machine-readable coverage and certificate-quality analysis for R-PREDA.

The analyzer is intentionally read-only: it consumes a schema-v5 relay
manifest and, optionally, a strict Native trace report.  It does not infer a
certificate from runtime observations and it never upgrades a conservative
or unsupported compiler result to ``Proved``.
"""

from __future__ import annotations

import collections
import itertools
import json
import pathlib
import re
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Set, Tuple


REQUESTED_PROPERTIES = (
    "MutualExclusive",
    "MustPrecede",
    "CoEmissionIndependent",
    "WorkBound",
    "DepthBound",
)

REQUESTED_STATUSES = ("Proved", "Unknown", "Unsupported")

UNKNOWN_REASON_RULES: Sequence[Tuple[str, Sequence[str]]] = (
    ("loop", ("loop", "occurrence", "trip count", "induction-variable")),
    ("recursion", ("recursive", "recursion", "cycle", "cyclic")),
    ("unresolved_call", ("unresolved call", "unresolved callee", "unresolved handler")),
    ("unsupported_formula", ("unsupported formula", "formula is unknown", "encoding error", "unknown formula")),
    ("unknown_effect", ("unknown effect", "external effect", "early-exit", "early exit", "unknown path", "may bypass")),
    ("ambiguous_call_context", ("ambiguous call", "ambiguous synchronous", "call context")),
    ("solver_timeout", ("solver timeout", "timed out", "timeout", "z3 returned unknown", "solver unknown")),
    ("unsupported_scope_relation", ("scope relation", "all-shards", "all shards", "partition-disjoint", "global or", "broadcast destination")),
)

UNKNOWN_REASON_CATEGORIES: Tuple[str, ...] = tuple(
    category for category, _ in UNKNOWN_REASON_RULES
) + ("other_conservative",)


class ManifestValidationError(ValueError):
    pass


def validate_manifest(
    manifest: Mapping[str, Any], *, require_artifact_binding: bool = True
) -> None:
    diagnostics: List[str] = []
    if type(manifest.get("schema_version")) is not int or manifest.get("schema_version") != 5:
        diagnostics.append("manifest schema_version must be 5")
    binding = manifest.get("artifact_binding")
    if require_artifact_binding and (
        not isinstance(binding, Mapping) or binding.get("binding_complete") is not True
    ):
        diagnostics.append("artifact_binding.binding_complete must be true")
    elif require_artifact_binding and isinstance(binding, Mapping):
        for field in (
            "dapp", "contract", "transpiler_version", "intermediate_hash",
            "module_id", "module_hash_kind", "module_hash",
            "manifest_hash_algorithm", "manifest_hash",
        ):
            if not isinstance(binding.get(field), str) or not binding.get(field):
                diagnostics.append("artifact_binding.%s must be a non-empty string" % field)
        if binding.get("manifest_hash_algorithm") != "sha256":
            diagnostics.append("artifact_binding.manifest_hash_algorithm must be sha256")
    control_flow = manifest.get("control_flow")
    if not isinstance(control_flow, Mapping) or type(control_flow.get("extension_schema_version")) is not int or control_flow.get("extension_schema_version") != 1:
        diagnostics.append("control_flow.extension_schema_version must be 1")
    certificate = manifest.get("parallel_certificate")
    if not isinstance(certificate, Mapping) or type(certificate.get("extension_schema_version")) is not int or certificate.get("extension_schema_version") != 1:
        diagnostics.append("parallel_certificate.extension_schema_version must be 1")

    refinement = manifest.get("refinement")
    if not isinstance(refinement, Mapping):
        diagnostics.append("refinement must be an object")
        refinement = {}

    def mapping_array(container: Mapping[str, Any], field: str, label: str) -> List[Mapping[str, Any]]:
        raw = container.get(field)
        if not isinstance(raw, list):
            diagnostics.append("%s must be an array" % label)
            return []
        if any(not isinstance(item, Mapping) for item in raw):
            diagnostics.append("%s must contain only objects" % label)
        return [item for item in raw if isinstance(item, Mapping)]

    sites = mapping_array(manifest, "relay_sites", "relay_sites")
    handlers = mapping_array(manifest, "handlers", "handlers")
    protocol_edges = mapping_array(manifest, "edges", "edges")
    protocol_functions = mapping_array(manifest, "functions", "functions")
    symbols = mapping_array(refinement, "symbols", "refinement.symbols")
    constraints = mapping_array(refinement, "constraints", "refinement.constraints")
    obligations = mapping_array(refinement, "proof_obligations", "refinement.proof_obligations")
    if not isinstance(control_flow, Mapping):
        control_flow = {}
    cfg_functions = mapping_array(control_flow, "functions", "control_flow.functions")
    if not isinstance(certificate, Mapping):
        certificate = {}
    certificate_functions = mapping_array(certificate, "functions", "parallel_certificate.functions")

    collections_with_ids = (
        ("relay site", sites, "id"),
        ("handler", handlers, "id"),
        ("protocol edge", protocol_edges, "id"),
        ("protocol function", protocol_functions, "source_function_id"),
        ("CFG function", cfg_functions, "function_id"),
        ("certificate function", certificate_functions, "source_function_id"),
        ("refinement symbol", symbols, "id"),
        ("constraint", constraints, "id"),
        ("proof obligation", obligations, "id"),
    )
    for label, values, id_field in collections_with_ids:
        ids = [_string(value.get(id_field)) for value in values]
        if any(not value for value in ids):
            diagnostics.append("%s collection contains an empty id" % label)
        duplicates = sorted(value for value, count in collections.Counter(ids).items() if value and count != 1)
        if duplicates:
            diagnostics.append("duplicate %s ids: %s" % (label, ",".join(duplicates)))

    site_ids = {_string(value.get("id")) for value in sites}
    sites_by_id = {_string(value.get("id")): value for value in sites}
    handler_ids = {_string(value.get("id")) for value in handlers}
    protocol_function_ids = {
        _string(value.get("source_function_id")) for value in protocol_functions
    }
    cfg_function_ids = {_string(value.get("function_id")) for value in cfg_functions}
    constraint_ids = {_string(value.get("id")) for value in constraints}
    symbol_sorts = {
        _string(value.get("id")): value.get("sort")
        for value in symbols
        if _string(value.get("id"))
    }

    for symbol in symbols:
        symbol_id = _string(symbol.get("id"))
        errors = _formula_sort_errors(symbol.get("sort"))
        diagnostics.extend("refinement symbol %s: %s" % (symbol_id, error) for error in errors)

    declared_sites_by_function: Dict[str, Set[str]] = {}
    for function in protocol_functions:
        function_id = _string(function.get("source_function_id"))
        if function_id not in cfg_function_ids:
            diagnostics.append("protocol function references an unknown CFG function: %s" % function_id)
        raw_site_ids = function.get("relay_site_ids")
        if not isinstance(raw_site_ids, list) or any(not isinstance(value, str) or not value for value in raw_site_ids):
            diagnostics.append("protocol function relay_site_ids must contain only non-empty strings: %s" % function_id)
            raw_site_ids = []
        if len(set(raw_site_ids)) != len(raw_site_ids):
            diagnostics.append("protocol function contains duplicate relay_site_ids: %s" % function_id)
        declared_sites_by_function[function_id] = set(raw_site_ids)
        for site_id in raw_site_ids:
            site = sites_by_id.get(site_id)
            if site is None:
                diagnostics.append("protocol function references an unknown relay site: %s" % site_id)
            elif _string(site.get("source_function_id")) != function_id:
                diagnostics.append("protocol function owns a relay site from another function: %s" % site_id)

    for site in sites:
        site_id = _string(site.get("id"))
        function_id = _string(site.get("source_function_id"))
        handler_id = _string(site.get("handler_id"))
        if function_id not in cfg_function_ids or function_id not in protocol_function_ids:
            diagnostics.append("relay site references an unknown source function: %s" % function_id)
        if site_id not in declared_sites_by_function.get(function_id, set()):
            diagnostics.append("relay site is absent from its protocol function relay_site_ids: %s" % site_id)
        if handler_id not in handler_ids:
            diagnostics.append("relay site references an unknown handler_id: %s" % handler_id)

    for edge in protocol_edges:
        site_id = _string(edge.get("relay_site_id"))
        handler_id = _string(edge.get("handler_id"))
        function_id = _string(edge.get("source_function_id"))
        if site_id not in site_ids:
            diagnostics.append("protocol edge references an unknown relay_site_id: %s" % site_id)
        if handler_id not in handler_ids:
            diagnostics.append("protocol edge references an unknown handler_id: %s" % handler_id)
        if function_id not in cfg_function_ids:
            diagnostics.append("protocol edge references an unknown source_function_id: %s" % function_id)
        site = sites_by_id.get(site_id, {})
        if site and _string(site.get("source_function_id")) != function_id:
            diagnostics.append("protocol edge source_function_id does not own relay site: %s" % site_id)
        if site and _string(site.get("handler_id")) != handler_id:
            diagnostics.append("protocol edge handler_id does not match relay site: %s" % site_id)
        if type(edge.get("resolved")) is not bool:
            diagnostics.append("protocol edge resolved must be Boolean: %s" % _string(edge.get("id")))
    for handler in handlers:
        if type(handler.get("resolved")) is not bool:
            diagnostics.append("handler resolved must be Boolean: %s" % _string(handler.get("id")))
        if handler.get("resolved") is True and _string(handler.get("target_function_id")) not in cfg_function_ids:
            diagnostics.append("resolved handler references an unknown target_function_id: %s" % _string(handler.get("target_function_id")))
    for function in cfg_functions:
        nodes = mapping_array(function, "nodes", "control_flow.functions[].nodes")
        node_ids = [_string(value.get("id")) for value in nodes]
        duplicates = [value for value, count in collections.Counter(node_ids).items() if value and count > 1]
        if any(not value for value in node_ids) or duplicates:
            diagnostics.append("CFG function contains empty or duplicate node IDs: %s" % _string(function.get("function_id")))
        for node in nodes:
            if _string(node.get("kind")) == "RelayEmit" and _string(node.get("relay_site_id")) not in site_ids:
                diagnostics.append("CFG RelayEmit references an unknown relay site: %s" % _string(node.get("relay_site_id")))

    for constraint in constraints:
        function_id = _string(constraint.get("source_function_id"))
        site_id = _string(constraint.get("relay_site_id"))
        if function_id and function_id not in cfg_function_ids:
            diagnostics.append("constraint references an unknown source_function_id: %s" % function_id)
        if site_id and site_id not in site_ids:
            diagnostics.append("constraint references an unknown relay_site_id: %s" % site_id)
        errors = _formula_validation_errors(constraint.get("formula"), symbol_sorts)
        diagnostics.extend("constraint %s formula: %s" % (_string(constraint.get("id")), error) for error in errors)

    for obligation in obligations:
        function_id = _string(obligation.get("source_function_id"))
        site_id = _string(obligation.get("relay_site_id"))
        related_site_id = _string(obligation.get("related_relay_site_id"))
        if function_id and function_id not in cfg_function_ids:
            diagnostics.append("proof obligation references an unknown source_function_id: %s" % function_id)
        if site_id and site_id not in site_ids:
            diagnostics.append("proof obligation references an unknown relay_site_id: %s" % site_id)
        if related_site_id and related_site_id not in site_ids:
            diagnostics.append("proof obligation references an unknown related_relay_site_id: %s" % related_site_id)
        obligation_constraint_ids = obligation.get("constraint_ids")
        if not isinstance(obligation_constraint_ids, list) or any(not isinstance(value, str) for value in obligation_constraint_ids):
            diagnostics.append("proof obligation constraint_ids must be an array of strings: %s" % _string(obligation.get("id")))
            obligation_constraint_ids = []
        dangling = sorted(set(obligation_constraint_ids).difference(constraint_ids))
        if dangling:
            diagnostics.append("proof obligation references unknown constraint IDs: %s" % ",".join(dangling))
        errors = _formula_validation_errors(obligation.get("goal"), symbol_sorts)
        diagnostics.extend("proof obligation %s goal: %s" % (_string(obligation.get("id")), error) for error in errors)

    semantic_obligation_kinds = {
        "RelayTargetEquality", "RelayArgumentEquality", "RelayGuardNecessity",
        "RelayGuardEquivalence", "RelayCountEquality", "RelayCountUpperBound",
        "RelayMutualExclusion", "RelayTargetIndependence", "TargetNonAliasCandidate",
    }
    semantic_constraint_kinds = {
        "RelayTargetRelation", "RelayArgumentRelation", "RelayGuardRelation",
        "RelayCountEquality", "RelayCountUpperBound", "RelayCountNonNegative",
    }

    def semantic_key(value: Mapping[str, Any]) -> Tuple[Any, ...]:
        site_a = _string(value.get("relay_site_id"))
        site_b = _string(value.get("related_relay_site_id"))
        pair = tuple(sorted((site_a, site_b))) if site_b else (site_a, "")
        argument_index = value.get("argument_index") if "Argument" in _string(value.get("kind")) else None
        return (_string(value.get("kind")), _string(value.get("source_function_id")), pair, argument_index)

    for label, values, kinds in (
        ("proof obligation", obligations, semantic_obligation_kinds),
        ("constraint", constraints, semantic_constraint_kinds),
    ):
        keys = [semantic_key(value) for value in values if _string(value.get("kind")) in kinds]
        duplicate_keys = [key for key, count in collections.Counter(keys).items() if count > 1]
        if duplicate_keys:
            diagnostics.append("duplicate %s semantic keys: %s" % (label, repr(sorted(duplicate_keys))))

    certificate_ids: List[str] = []
    pair_semantic_keys: List[Tuple[str, str, str, str]] = []
    allowed_pair_relations = {
        "MutuallyExclusive", "CoEmissionIndependent", "ProvedMayAlias",
        "PotentialConflict", "Unknown", "MustPrecedeAB", "MustPrecedeBA",
    }
    allowed_pair_statuses = {"Proved", "Complete", "Conservative", "Unknown", "Unsupported"}
    for function in certificate_functions:
        function_id = _string(function.get("source_function_id"))
        if function_id not in cfg_function_ids:
            diagnostics.append("parallel certificate references an unknown source function: %s" % function_id)
        for field in ("direct_logical_work", "transitive_logical_work", "physical_route_work", "relay_tree_depth"):
            value = function.get(field)
            if not isinstance(value, Mapping):
                diagnostics.append("parallel certificate omits %s for %s" % (field, _string(function.get("source_function_id"))))
                continue
            certificate_ids.append(_string(value.get("certificate_id")))
        for pair in mapping_array(function, "pair_relations", "parallel_certificate.functions[].pair_relations"):
            certificate_ids.append(_string(pair.get("certificate_id")))
            pair_function_id = _string(pair.get("source_function_id"))
            site_a, site_b = _string(pair.get("site_a")), _string(pair.get("site_b"))
            relation, status = _string(pair.get("relation")), _string(pair.get("status"))
            if pair_function_id != function_id:
                diagnostics.append("parallel pair source_function_id does not match certificate owner: %s" % _string(pair.get("certificate_id")))
            if site_a not in site_ids or site_b not in site_ids:
                diagnostics.append("parallel pair references an unknown relay site: %s/%s" % (site_a, site_b))
            if site_a == site_b:
                diagnostics.append("parallel pair must reference two distinct relay sites: %s" % site_a)
            if relation not in allowed_pair_relations:
                diagnostics.append("parallel pair has an unknown relation: %s" % relation)
            if status not in allowed_pair_statuses:
                diagnostics.append("parallel pair has an unknown status: %s" % status)
            relation_class = "precedence" if relation in ("MustPrecedeAB", "MustPrecedeBA") else "classification"
            pair_semantic_keys.append((function_id, *sorted((site_a, site_b)), relation_class))
    duplicate_certificate_ids = sorted(value for value, count in collections.Counter(certificate_ids).items() if value and count > 1)
    if any(not value for value in certificate_ids):
        diagnostics.append("parallel certificate contains an empty certificate_id")
    if duplicate_certificate_ids:
        diagnostics.append("duplicate certificate IDs: %s" % ",".join(duplicate_certificate_ids))
    duplicate_pair_keys = sorted(key for key, count in collections.Counter(pair_semantic_keys).items() if count > 1)
    if duplicate_pair_keys:
        diagnostics.append("duplicate parallel pair semantic keys: %s" % repr(duplicate_pair_keys))
    if diagnostics:
        raise ManifestValidationError("; ".join(diagnostics))


def _mapping_list(value: Any) -> List[Mapping[str, Any]]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, Mapping)]


def _string(value: Any) -> str:
    return "" if value is None else str(value)


def _percent(numerator: int, denominator: int) -> Optional[float]:
    if denominator == 0:
        return None
    return round(100.0 * float(numerator) / float(denominator), 3)


def _expr_finite(value: Any) -> bool:
    if not isinstance(value, Mapping) or _string(value.get("kind")).lower() in ("", "unknown"):
        return False
    for child in value.values():
        if isinstance(child, Mapping) and "kind" in child and not _expr_finite(child):
            return False
        if isinstance(child, list):
            for item in child:
                if isinstance(item, Mapping) and "kind" in item and not _expr_finite(item):
                    return False
    return True


FORMULA_KINDS = {
    "BoolLiteral", "IntLiteral", "BitVectorLiteral", "AddressLiteral",
    "Symbol", "Group", "Unary", "Binary", "Nary", "Cast", "Ite",
    "ArrayLength", "Unknown",
}
FORMULA_SORTS = {"Bool", "Int", "UnsignedBitVector", "Address", "Unknown"}
BIT_VECTOR_WIDTHS = {8, 16, 32, 64, 96, 128, 160, 256, 512}


def _formula_sort_key(value: Any) -> Optional[Tuple[str, int]]:
    if not isinstance(value, Mapping):
        return None
    kind = _string(value.get("kind"))
    if kind not in FORMULA_SORTS:
        return None
    if kind != "UnsignedBitVector":
        return kind, 0
    width = value.get("bit_width", value.get("bitWidth"))
    if type(width) is not int or width not in BIT_VECTOR_WIDTHS:
        return None
    return kind, width


def _formula_sort_errors(value: Any) -> List[str]:
    return [] if _formula_sort_key(value) is not None else ["invalid or unsupported formula sort"]


def _formula_validation_errors(
    value: Any,
    symbol_sorts: Mapping[str, Any],
    path: str = "root",
) -> List[str]:
    if not isinstance(value, Mapping):
        return ["%s must be an object" % path]
    errors: List[str] = []
    kind = _string(value.get("kind"))
    if kind not in FORMULA_KINDS:
        errors.append("%s has an unknown formula kind %r" % (path, kind))
    sort_key = _formula_sort_key(value.get("sort"))
    if sort_key is None:
        errors.append("%s has an invalid formula sort" % path)
    children = value.get("children")
    if not isinstance(children, list) or any(not isinstance(child, Mapping) for child in children):
        errors.append("%s.children must contain only formula objects" % path)
        children = []

    exact_arity = {
        "BoolLiteral": 0,
        "IntLiteral": 0,
        "BitVectorLiteral": 0,
        "AddressLiteral": 0,
        "Symbol": 0,
        "Unknown": 0,
        "Group": 1,
        "Unary": 1,
        "Cast": 1,
        "ArrayLength": 1,
        "Binary": 2,
        "Ite": 3,
    }
    if kind in exact_arity and len(children) != exact_arity[kind]:
        errors.append("%s kind %s requires %d children" % (path, kind, exact_arity[kind]))
    if kind == "Nary" and len(children) < 2:
        errors.append("%s kind Nary requires at least two children" % path)

    if kind == "Symbol":
        symbol_id = value.get("symbol_id")
        if not isinstance(symbol_id, str) or not symbol_id:
            errors.append("%s Symbol requires a non-empty symbol_id" % path)
        elif symbol_id not in symbol_sorts:
            errors.append("%s Symbol references an unknown symbol_id %s" % (path, symbol_id))
        elif sort_key != _formula_sort_key(symbol_sorts[symbol_id]):
            errors.append("%s Symbol sort does not match its declaration" % path)
    if kind in ("BoolLiteral", "IntLiteral", "BitVectorLiteral", "AddressLiteral"):
        literal = value.get("literal_value")
        if not isinstance(literal, (str, int, bool)) or isinstance(literal, str) and not literal:
            errors.append("%s %s requires literal_value" % (path, kind))
        expected_sort = {
            "BoolLiteral": "Bool",
            "IntLiteral": "Int",
            "BitVectorLiteral": "UnsignedBitVector",
            "AddressLiteral": "Address",
        }[kind]
        if sort_key is not None and sort_key[0] != expected_sort:
            errors.append("%s %s has an incompatible sort" % (path, kind))
    if kind in ("Unary", "Binary", "Nary", "Cast", "Ite", "ArrayLength"):
        if not isinstance(value.get("operator"), str) or not value.get("operator"):
            errors.append("%s kind %s requires an operator" % (path, kind))

    for index, child in enumerate(children):
        errors.extend(_formula_validation_errors(child, symbol_sorts, "%s.children[%d]" % (path, index)))
    return errors


def _formula_contains_unknown(value: Any, symbol_sorts: Mapping[str, Any]) -> bool:
    if _formula_validation_errors(value, symbol_sorts):
        return True
    if not isinstance(value, Mapping):
        return True
    kind = _string(value.get("kind"))
    sort_key = _formula_sort_key(value.get("sort"))
    if kind == "Unknown" or sort_key is None or sort_key[0] == "Unknown":
        return True
    return any(_formula_contains_unknown(child, symbol_sorts) for child in value.get("children", []))


def classify_unknown_reason(reason: str) -> str:
    normalized = reason.strip().lower()
    for category, needles in UNKNOWN_REASON_RULES:
        if any(needle in normalized for needle in needles):
            return category
    return "other_conservative"


def classify_unknown_reasons(reasons: Sequence[str]) -> List[str]:
    categories: List[str] = []
    for reason in reasons:
        category = classify_unknown_reason(reason)
        if category not in categories:
            categories.append(category)
    priority = [category for category, _ in UNKNOWN_REASON_RULES] + ["other_conservative"]
    return sorted(categories or ["other_conservative"], key=lambda value: priority.index(value) if value in priority else len(priority))


def _nested_unknown_reasons(value: Any) -> List[str]:
    result: List[str] = []
    if isinstance(value, Mapping):
        if _string(value.get("kind")).lower() == "unknown" and _string(value.get("reason")):
            result.append(_string(value.get("reason")))
        for child in value.values():
            result.extend(_nested_unknown_reasons(child))
    elif isinstance(value, list):
        for child in value:
            result.extend(_nested_unknown_reasons(child))
    return list(dict.fromkeys(result))


def _structured_ids(value: Any) -> Set[str]:
    result: Set[str] = set()
    if isinstance(value, Mapping):
        for key, child in value.items():
            if (key == "id" or key.endswith("_id")) and isinstance(child, str) and child:
                result.add(child)
            result.update(_structured_ids(child))
    elif isinstance(value, list):
        for child in value:
            result.update(_structured_ids(child))
    return result


def _normalize_status(raw: Any, *, finite_bound: bool = False) -> str:
    value = _string(raw)
    if value == "Unsupported":
        return "Unsupported"
    # Complete/Proved status cannot substitute for an owning finite formula.
    # A conservative finite upper bound is nevertheless a sound proved bound.
    if finite_bound and value in ("Proved", "Complete", "Conservative"):
        return "Proved"
    return "Unknown"


def _evidence(item: Mapping[str, Any]) -> Mapping[str, Any]:
    return {
        "cfg_fact_ids": list(item.get("supporting_cfg_fact_ids", [])),
        "constraint_ids": list(item.get("supporting_constraint_ids", [])),
        "solver_result_ids": list(item.get("supporting_solver_result_ids", [])),
    }


def _selected_function_ids(
    manifest: Mapping[str, Any], function_filters: Sequence[str]
) -> Set[str]:
    cfg_functions = _mapping_list(manifest.get("control_flow", {}).get("functions", []))
    all_ids = {_string(function.get("function_id")) for function in cfg_functions}
    all_ids.discard("")
    if not function_filters:
        return all_ids

    unmatched = [
        pattern
        for pattern in function_filters
        if not any(pattern == function_id or pattern in function_id for function_id in all_ids)
    ]
    if unmatched:
        raise ManifestValidationError(
            "function filters matched no function: %s" % ",".join(unmatched)
        )

    selected = {
        function_id
        for function_id in all_ids
        if any(pattern == function_id or pattern in function_id for pattern in function_filters)
    }
    handlers = {
        _string(handler.get("id")): _string(handler.get("target_function_id"))
        for handler in _mapping_list(manifest.get("handlers", []))
    }
    synchronous_callees: MutableMapping[str, Set[str]] = collections.defaultdict(set)
    for edge in _mapping_list(
        manifest.get("control_flow", {}).get("synchronous_call_graph", {}).get("edges", [])
    ):
        caller, callee = _string(edge.get("caller")), _string(edge.get("callee"))
        if (
            _string(edge.get("kind")) == "Synchronous"
            and bool(edge.get("resolved"))
            and caller in all_ids
            and callee in all_ids
        ):
            synchronous_callees[caller].add(callee)
    # A workload slice includes both resolved synchronous callees and the
    # async-handler closure. Otherwise helper-owned relay emissions disappear
    # from static and runtime denominators.
    changed = True
    while changed:
        changed = False
        for caller in list(selected):
            for callee in synchronous_callees.get(caller, set()):
                if callee not in selected:
                    selected.add(callee)
                    changed = True
        for edge in _mapping_list(manifest.get("edges", [])):
            if _string(edge.get("source_function_id")) not in selected:
                continue
            target = handlers.get(_string(edge.get("handler_id")), "")
            if target and target in all_ids and target not in selected:
                selected.add(target)
                changed = True
    return selected


def _obligation_is_supported(
    obligation: Mapping[str, Any], symbol_sorts: Mapping[str, Any]
) -> bool:
    if _string(obligation.get("status")) == "Unsupported":
        return False
    goal = obligation.get("goal")
    return isinstance(goal, Mapping) and not _formula_contains_unknown(goal, symbol_sorts)


def _coverage_count(supported: int, eligible: int, ids: Sequence[str]) -> Mapping[str, Any]:
    return {
        "eligible": eligible,
        "covered": supported,
        "percentage": _percent(supported, eligible),
        "evidence_ids": list(ids),
    }


def _certificate_records(
    manifest: Mapping[str, Any], selected: Set[str], protocol_function_ids: Set[str]
) -> List[Mapping[str, Any]]:
    records: List[Mapping[str, Any]] = []
    cfg_ids = _structured_ids(manifest.get("control_flow", {}))
    constraint_ids = {
        _string(value.get("id"))
        for value in _mapping_list(manifest.get("refinement", {}).get("constraints", []))
    }
    solver_results = {
        _string(value.get("id")): value
        for value in _mapping_list(manifest.get("refinement", {}).get("proof_obligations", []))
    }
    solver_ids = set(solver_results)

    def make_record(
        prop: str,
        function_id: str,
        value: Mapping[str, Any],
        outcome: str,
        *,
        subproperty: str = "",
        site_a: str = "",
        site_b: str = "",
        relation: str = "",
        strength: str = "",
    ) -> Mapping[str, Any]:
        evidence = _evidence(value)
        invalid_reference = (
            any(item not in cfg_ids for item in evidence["cfg_fact_ids"])
            or any(item not in constraint_ids for item in evidence["constraint_ids"])
            or any(item not in solver_ids for item in evidence["solver_result_ids"])
        )
        evidence_nonempty = any(evidence.values())
        evidence_reason = ""
        strong_outcome = outcome in ("Proved", "Disproved")
        if strong_outcome and not invalid_reference:
            referenced_solver_results = [
                solver_results[item]
                for item in evidence["solver_result_ids"]
                if item in solver_results
            ]

            def has_solver_result(kind: str, status: str) -> bool:
                return any(
                    _string(item.get("kind")) == kind
                    and _string(item.get("solver_result", {}).get("status")) == status
                    for item in referenced_solver_results
                    if isinstance(item.get("solver_result", {}), Mapping)
                )

            if outcome == "Proved" and prop == "MutualExclusive" and not has_solver_result(
                "RelayMutualExclusion", "Proved"
            ):
                evidence_reason = "MutualExclusive lacks a Proved RelayMutualExclusion solver result"
            elif outcome == "Proved" and prop == "CoEmissionIndependent" and not (
                has_solver_result("RelayMutualExclusion", "Disproved")
                and has_solver_result("RelayTargetIndependence", "Proved")
            ):
                evidence_reason = "CoEmissionIndependent lacks the required Disproved co-emission and Proved target-independence results"
            elif outcome == "Proved" and prop == "MustPrecede" and not evidence["cfg_fact_ids"]:
                evidence_reason = "MustPrecede lacks CFG evidence"
            elif outcome == "Proved" and prop in ("WorkBound", "DepthBound") and function_id not in evidence["cfg_fact_ids"]:
                evidence_reason = "%s evidence is not anchored to its source function" % prop
            elif outcome == "Disproved" and prop == "MutualExclusive" and not has_solver_result(
                "RelayMutualExclusion", "Disproved"
            ):
                evidence_reason = "disproved MutualExclusive lacks a Disproved RelayMutualExclusion result"
            elif outcome == "Disproved" and prop == "CoEmissionIndependent" and not (
                has_solver_result("RelayMutualExclusion", "Disproved")
                and has_solver_result("RelayTargetIndependence", "Disproved")
            ):
                evidence_reason = "disproved CoEmissionIndependent lacks Disproved co-emission and target-independence results"
        evidence_valid = (
            not invalid_reference
            and (not strong_outcome or evidence_nonempty)
            and not evidence_reason
        )
        if strong_outcome and not evidence_valid:
            outcome = "InvalidEvidence"
        raw_reasons = list(
            dict.fromkeys(
                [reason for reason in [_string(value.get("reason"))] if reason]
                + _nested_unknown_reasons(value.get("exact", {}))
                + _nested_unknown_reasons(value.get("upper_bound", {}))
                + ([evidence_reason] if evidence_reason else [])
                + (["one or more evidence IDs do not resolve"] if invalid_reference else [])
            )
        )
        categories = classify_unknown_reasons(raw_reasons) if outcome in ("Unknown", "Unsupported", "NotEstablished", "InvalidEvidence") else []
        return {
            "property": prop,
            "subproperty": subproperty,
            "status": outcome,
            "raw_status": _string(value.get("status")),
            "relation": relation,
            "proof_strength": strength,
            "benchmark_function_id": function_id,
            "certificate_id": _string(value.get("certificate_id") or value.get("id")),
            "site_a": site_a,
            "site_b": site_b,
            "reason": "; ".join(raw_reasons),
            "raw_reasons": raw_reasons,
            "reason_categories": categories,
            "unknown_reason": categories[0] if categories else "",
            "evidence": evidence,
            "evidence_valid": evidence_valid,
            "evidence_validation_reason": evidence_reason or (
                "one or more evidence IDs do not resolve" if invalid_reference else ""
            ),
        }

    functions = _mapping_list(manifest.get("parallel_certificate", {}).get("functions", []))
    for function in functions:
        function_id = _string(function.get("source_function_id"))
        if function_id not in selected or function_id not in protocol_function_ids:
            continue
        pair_groups: MutableMapping[Tuple[str, str], List[Mapping[str, Any]]] = collections.defaultdict(list)
        for pair in _mapping_list(function.get("pair_relations", [])):
            pair_groups[tuple(sorted((_string(pair.get("site_a")), _string(pair.get("site_b")))))] .append(pair)
        for (site_a, site_b), pair_values in sorted(pair_groups.items()):
            precedence = next((value for value in pair_values if _string(value.get("relation")) in ("MustPrecedeAB", "MustPrecedeBA")), None)
            classification = next((value for value in pair_values if _string(value.get("relation")) not in ("MustPrecedeAB", "MustPrecedeBA")), pair_values[0])
            relation = _string(classification.get("relation"))
            raw_status = _string(classification.get("status"))

            if relation == "MutuallyExclusive" and raw_status == "Proved":
                mutual = "Proved"
            elif relation in ("CoEmissionIndependent", "ProvedMayAlias", "PotentialConflict") and raw_status == "Proved":
                mutual = "Disproved"
            elif raw_status == "Unsupported":
                mutual = "Unsupported"
            else:
                mutual = "Unknown"
            records.append(make_record("MutualExclusive", function_id, classification, mutual, site_a=site_a, site_b=site_b, relation=relation, strength="Solver" if mutual in ("Proved", "Disproved") else ""))

            if precedence is not None and _string(precedence.get("status")) == "Proved":
                must = "Proved"
                must_value = precedence
            elif relation == "MutuallyExclusive":
                must = "NotApplicable"
                must_value = classification
            else:
                must = "NotEstablished"
                must_value = dict(classification)
                must_value["reason"] = "precedence attempt outcome is not persisted in schema-v5 when no MustPrecede certificate is produced"
            records.append(make_record("MustPrecede", function_id, must_value, must, site_a=site_a, site_b=site_b, relation=_string(must_value.get("relation")), strength="CFG" if must == "Proved" else ""))

            if relation == "CoEmissionIndependent" and raw_status == "Proved":
                independent = "Proved"
            elif relation == "ProvedMayAlias" and raw_status == "Proved":
                independent = "Disproved"
            elif relation == "MutuallyExclusive":
                independent = "NotApplicable"
            elif raw_status == "Unsupported":
                independent = "Unsupported"
            else:
                independent = "Unknown"
            records.append(make_record("CoEmissionIndependent", function_id, classification, independent, site_a=site_a, site_b=site_b, relation=relation, strength="Solver" if independent in ("Proved", "Disproved") else ""))

        for field, subproperty in (
            ("direct_logical_work", "DirectLogicalWork"),
            ("transitive_logical_work", "TransitiveLogicalWork"),
            ("physical_route_work", "PhysicalRouteWork"),
        ):
            value = function.get(field)
            value = value if isinstance(value, Mapping) else {}
            if field == "physical_route_work":
                # ``Constant`` describes the shape of an upper bound, not an
                # equality between the bound and observed route work.
                exact = _expr_finite(value.get("exact"))
                finite = _string(value.get("bound_kind")) in ("Constant", "ParameterizedUpperBound")
            else:
                exact = _expr_finite(value.get("exact"))
                finite = _expr_finite(value.get("upper_bound"))
            outcome = _normalize_status(value.get("status"), finite_bound=finite)
            strength = "Exact" if outcome == "Proved" and exact else ("FiniteUpperBound" if outcome == "Proved" else "")
            record = dict(make_record("WorkBound", function_id, value, outcome, subproperty=subproperty, strength=strength))
            if record["status"] != "Proved":
                record["proof_strength"] = ""
            raw_upper_bound = value.get("upper_bound")
            if isinstance(raw_upper_bound, Mapping):
                normalized_upper_bound = dict(raw_upper_bound)
            elif field == "physical_route_work":
                normalized_upper_bound = {
                    "kind": _string(value.get("bound_kind")) or "Unknown",
                    "expression": _string(value.get("expression")),
                    "constant_term": value.get("constant_term"),
                    "active_shard_count_coefficient": value.get(
                        "active_shard_count_coefficient"
                    ),
                    "reason": _string(value.get("reason")),
                }
            else:
                normalized_upper_bound = {
                    "kind": "Unknown",
                    "reason": _string(value.get("reason"))
                    or "upper bound is absent",
                }
            record.update({"finite_upper_bound": finite, "exact_known": exact, "upper_bound": normalized_upper_bound})
            records.append(record)

        value = function.get("relay_tree_depth", {})
        value = value if isinstance(value, Mapping) else {}
        exact = _expr_finite(value.get("exact"))
        finite = _expr_finite(value.get("upper_bound"))
        outcome = _normalize_status(value.get("status"), finite_bound=finite)
        strength = "Exact" if outcome == "Proved" and exact else ("FiniteUpperBound" if outcome == "Proved" else "")
        record = dict(make_record("DepthBound", function_id, value, outcome, strength=strength))
        if record["status"] != "Proved":
            record["proof_strength"] = ""
        record.update({"finite_upper_bound": finite, "exact_known": exact, "upper_bound": value.get("upper_bound", {})})
        records.append(record)
    return records


def _runtime_weighted(
    manifest: Mapping[str, Any], trace: Optional[Mapping[str, Any]], selected: Set[str]
) -> Mapping[str, Any]:
    def unavailable(reason: str) -> Mapping[str, Any]:
        return {
            "available": False,
            "reason": reason,
            "relay_emissions": {
                "observed": 0,
                "manifest_identity_validation_passed": 0,
                "manifest_identity_covered": 0,
                "certificate_covered": 0,
                "percentage": None,
                "details": [],
            },
            "relay_pairs": {
                "observed": 0,
                "observed_distinct_site": 0,
                "certified": 0,
                "percentage": None,
                "all_pair_instance_percentage": None,
                "details": [],
            },
            "relay_trees": {
                "executed": 0,
                "finite_work_bound": 0,
                "finite_depth_bound": 0,
                "work_percentage": None,
                "depth_percentage": None,
                "details": [],
            },
            "transactions": {
                "source_transactions": 0,
                "complete": 0,
                "partial": 0,
                "fallback": 0,
                "details": [],
            },
        }

    if not isinstance(trace, Mapping):
        return unavailable("no Native strict trace was supplied")
    if type(trace.get("report_schema_version")) is not int or trace.get("report_schema_version") != 1:
        return unavailable("Native trace report_schema_version must be 1")
    for field in (
        "logical_relay_emissions", "physical_relay_routes",
        "relay_executions", "validation_results",
    ):
        values = trace.get(field)
        if not isinstance(values, list) or any(not isinstance(value, Mapping) for value in values):
            return unavailable("Native trace %s must be an array of objects" % field)
    counters = trace.get("counters")
    if not isinstance(counters, Mapping):
        return unavailable("Native trace counters must be an object")
    executions_for_counters = _mapping_list(trace.get("relay_executions", []))
    counter_expectations = {
        "logical_relay_emissions": len(_mapping_list(trace.get("logical_relay_emissions", []))),
        "physical_relay_routes": len(_mapping_list(trace.get("physical_relay_routes", []))),
        "source_transactions_observed": sum(
            not bool(value.get("is_relay")) for value in executions_for_counters
        ),
        "relay_executions": sum(
            bool(value.get("is_relay")) for value in executions_for_counters
        ),
        "microtransactions_observed": len(executions_for_counters),
    }
    for field, expected in counter_expectations.items():
        value = counters.get(field)
        if type(value) is not int or value < 0 or value != expected:
            return unavailable(
                "Native trace counter %s does not match its event array" % field
            )
    instrumentation_failures = counters.get("instrumentation_failures")
    if type(instrumentation_failures) is not int or instrumentation_failures < 0:
        return unavailable(
            "Native trace counter instrumentation_failures must be a non-negative integer"
        )

    protocol_functions = {
        _string(function.get("source_function_id")): function
        for function in _mapping_list(manifest.get("functions", []))
    }
    certificates = {
        _string(function.get("source_function_id")): function
        for function in _mapping_list(manifest.get("parallel_certificate", {}).get("functions", []))
    }
    binding = manifest.get("artifact_binding", {})
    binding = binding if isinstance(binding, Mapping) else {}
    manifest_module_id = _string(binding.get("module_id"))
    executions = _mapping_list(trace.get("relay_executions", []))
    routes_all = _mapping_list(trace.get("physical_relay_routes", []))
    validations = _mapping_list(trace.get("validation_results", []))

    # Select the source workload from independently resolved execution records.
    # Once a root is selected, every emission in its tree belongs to the runtime
    # denominator.  In particular, an incomplete identity or a synchronous
    # helper's static source_function_id must never make an observation vanish.
    root_candidates = [
        item
        for item in executions
        if not bool(item.get("is_relay"))
        and int(item.get("trace_tx_id", 0)) == int(item.get("root_trace_tx_id", 0))
        and (
            _string(item.get("source_function_id")) in selected
            # An unresolved execution identity cannot be safely assigned to a
            # different workload slice. Retain it as a conservative fallback
            # instead of silently shrinking the transaction denominator.
            or (
                _string(item.get("source_function_id")) not in protocol_functions
                and _string(item.get("module_id")) == manifest_module_id
            )
        )
    ]
    roots: Dict[int, Mapping[str, Any]] = {}
    duplicate_root_ids: Set[int] = set()
    duplicate_root_records: List[Mapping[str, Any]] = []
    for item in root_candidates:
        root_id = int(item.get("trace_tx_id", 0))
        if root_id in roots:
            duplicate_root_ids.add(root_id)
            duplicate_root_records.append(item)
        else:
            roots[root_id] = item
    selected_root_ids = set(roots)
    emissions = [
        emission
        for emission in _mapping_list(trace.get("logical_relay_emissions", []))
        if int(emission.get("root_trace_tx_id", 0)) in selected_root_ids
    ]
    routes = [
        route
        for route in routes_all
        if int(route.get("root_trace_tx_id", 0)) in selected_root_ids
    ]

    execution_by_id: Dict[int, Mapping[str, Any]] = {}
    duplicate_execution_ids: Set[int] = set()
    for item in executions:
        trace_id = int(item.get("trace_tx_id", 0))
        if trace_id in execution_by_id:
            duplicate_execution_ids.add(trace_id)
        else:
            execution_by_id[trace_id] = item

    validation_index: MutableMapping[Tuple[str, int, int, str], List[Mapping[str, Any]]] = collections.defaultdict(list)
    for validation in validations:
        detail = validation.get("detail", {})
        if not isinstance(detail, Mapping):
            continue
        validation_index[
            (
                _string(validation.get("check_kind")),
                int(detail.get("root_trace_tx_id", 0)),
                int(detail.get("current_trace_tx_id", 0)),
                _string(detail.get("module_identity")),
            )
        ].append(validation)

    hard_statuses = (
        "Mismatch",
        "ManifestNotLoaded",
        "ManifestBindingMismatch",
        "TraceInstrumentationError",
    )

    def validation_status(values: Sequence[Mapping[str, Any]]) -> str:
        statuses = {_string(value.get("status")) for value in values if _string(value.get("status"))}
        if not statuses:
            return "Missing"
        for status in hard_statuses:
            if status in statuses:
                return status
        if statuses == {"Passed"}:
            return "Passed"
        if statuses == {"SkippedUnsupported"}:
            return "SkippedUnsupported"
        if statuses == {"NotApplicable"}:
            return "NotApplicable"
        # A Passed row cannot mask a duplicate Skipped/NotApplicable row for the
        # same certificate and execution context.
        return "ConflictingValidation"

    def matching_validations(
        check_kind: str,
        root_id: int,
        current_id: int,
        module_id: str,
        *,
        function_id: Optional[str] = None,
        certificate_id: Optional[str] = None,
    ) -> List[Mapping[str, Any]]:
        result = list(validation_index.get((check_kind, root_id, current_id, module_id), []))
        if function_id is not None:
            result = [
                value for value in result
                if _string(value.get("detail", {}).get("function")) == function_id
            ]
        if certificate_id is not None:
            result = [
                value for value in result
                if _string(value.get("detail", {}).get("certificate_id")) == certificate_id
            ]
        return result

    cfg_ids = _structured_ids(manifest.get("control_flow", {}))
    constraint_ids = {
        _string(value.get("id"))
        for value in _mapping_list(manifest.get("refinement", {}).get("constraints", []))
        if _string(value.get("id"))
    }
    solver_results = {
        _string(value.get("id")): value
        for value in _mapping_list(manifest.get("refinement", {}).get("proof_obligations", []))
        if _string(value.get("id"))
    }
    solver_ids = set(solver_results)

    def certificate_evidence(value: Mapping[str, Any]) -> Mapping[str, Any]:
        return {
            "cfg_fact_ids": [_string(item) for item in value.get("supporting_cfg_fact_ids", []) if _string(item)],
            "constraint_ids": [_string(item) for item in value.get("supporting_constraint_ids", []) if _string(item)],
            "solver_result_ids": [_string(item) for item in value.get("supporting_solver_result_ids", []) if _string(item)],
        }

    def certificate_evidence_valid(
        value: Mapping[str, Any], function_id: str = ""
    ) -> bool:
        evidence = certificate_evidence(value)
        return (
            any(evidence.values())
            and (not function_id or function_id in evidence["cfg_fact_ids"])
            and all(item in cfg_ids for item in evidence["cfg_fact_ids"])
            and all(item in constraint_ids for item in evidence["constraint_ids"])
            and all(item in solver_ids for item in evidence["solver_result_ids"])
        )

    bound_aliases = {
        "direct_logical_work": "direct_work_bound",
        "transitive_logical_work": "transitive_work_bound",
        "physical_route_work": "physical_work_bound",
        "relay_tree_depth": "depth_bound",
    }
    check_kinds = {
        "direct_logical_work": "certificate_direct_work",
        "transitive_logical_work": "certificate_transitive_work",
        "physical_route_work": "certificate_physical_work",
        "relay_tree_depth": "certificate_depth",
    }

    def static_bound(function_id: str, field: str) -> Mapping[str, Any]:
        certificate = certificates.get(function_id, {})
        value = certificate.get(field, certificate.get(bound_aliases[field], {}))
        return value if isinstance(value, Mapping) else {}

    def finite_static_bound(value: Mapping[str, Any], field: str) -> bool:
        if field == "physical_route_work":
            return _string(value.get("bound_kind")) in ("Constant", "ParameterizedUpperBound")
        return _expr_finite(value.get("upper_bound"))

    def static_bound_support(function_id: str, field: str) -> Mapping[str, Any]:
        value = static_bound(function_id, field)
        raw_status = _string(value.get("status"))
        certificate_id = _string(value.get("certificate_id") or value.get("id"))
        evidence = certificate_evidence(value)
        raw_reasons = list(dict.fromkeys(
            [reason for reason in [_string(value.get("reason"))] if reason]
            + _nested_unknown_reasons(value.get("exact", {}))
            + _nested_unknown_reasons(value.get("upper_bound", {}))
        ))
        if not value:
            status, reason = "StaticMissing", "static bound certificate is absent"
        elif raw_status not in ("Complete", "Conservative", "Proved") or not finite_static_bound(value, field):
            status = "StaticUnknown"
            reason = "; ".join(raw_reasons) or "static certificate has no finite established bound"
        elif not certificate_id:
            status, reason = "MissingCertificateIdentity", "static bound has no certificate ID"
        elif not certificate_evidence_valid(value, function_id):
            status, reason = "InvalidEvidence", "static bound evidence is empty or dangling"
        else:
            status, reason = "Usable", ""
        return {
            "usable": status == "Usable",
            "static_status": status,
            "raw_static_status": raw_status,
            "certificate_id": certificate_id,
            "evidence": evidence,
            "reason": reason,
            "raw_static_reasons": raw_reasons,
        }

    def checked_bound(
        function_id: str,
        field: str,
        root_id: int,
        current_id: int,
        module_id: str,
    ) -> Tuple[str, Mapping[str, Any]]:
        support = dict(static_bound_support(function_id, field))
        if module_id != manifest_module_id:
            support.update({"runtime_status": "ForeignModule", "reason": "no static manifest was supplied for this module"})
            return "ForeignModule", support
        if not support["usable"]:
            support["runtime_status"] = "NotChecked"
            return _string(support["static_status"]), support
        values = matching_validations(
            check_kinds[field], root_id, current_id, module_id,
            function_id=function_id,
            certificate_id=_string(support["certificate_id"]),
        )
        status = validation_status(values)
        support.update(
            {
                "runtime_status": status,
                "validation_reasons": list(dict.fromkeys(_string(value.get("reason")) for value in values if _string(value.get("reason")))),
                "validation_property_ids": list(dict.fromkeys(_string(value.get("detail", {}).get("property_id")) for value in values if _string(value.get("detail", {}).get("property_id")))),
            }
        )
        return status, support

    def identity_values(item: Mapping[str, Any]) -> List[Mapping[str, Any]]:
        root_id = int(item.get("root_trace_tx_id", 0))
        parent_id = int(item.get("parent_trace_tx_id", 0))
        module_id = _string(item.get("source_module_id"))
        result: List[Mapping[str, Any]] = []
        for value in matching_validations("relay_site_identity", root_id, parent_id, module_id):
            detail = value.get("detail", {})
            if (
                _string(detail.get("relay_site_id")) == _string(item.get("relay_site_id"))
                and int(detail.get("relay_site_ordinal", -1)) == int(item.get("relay_site_ordinal", -2))
                and int(detail.get("occurrence_index", -1)) == int(item.get("occurrence_index", -2))
            ):
                result.append(value)
        return result

    emission_details: List[Mapping[str, Any]] = []
    emission_covered = 0
    for item in emissions:
        root_id = int(item.get("root_trace_tx_id", 0))
        parent_id = int(item.get("parent_trace_tx_id", 0))
        function_id = _string(item.get("source_function_id"))
        module_id = _string(item.get("source_module_id"))
        identities = identity_values(item)
        identity_status = validation_status(identities)
        parent_execution = execution_by_id.get(parent_id, {})
        owner_function = (
            _string(parent_execution.get("source_function_id"))
            if _string(parent_execution.get("module_id")) == module_id
            else ""
        )
        work_status, work_support = checked_bound(
            owner_function, "direct_logical_work", root_id, parent_id, module_id
        )
        covered = identity_status == "Passed" and work_status == "Passed"
        emission_covered += int(covered)
        emission_details.append(
            {
                "root_trace_tx_id": root_id,
                "parent_trace_tx_id": parent_id,
                "source_function_id": function_id,
                "module_id": module_id,
                "relay_site_id": _string(item.get("relay_site_id")),
                "covered": covered,
                "identity_status": identity_status,
                "certificate_owner_function_id": owner_function,
                "direct_work_status": work_status,
                "direct_work_certificate_id": work_support.get("certificate_id", ""),
                "direct_work_evidence": work_support.get("evidence", {}),
                "reasons": list(dict.fromkeys(
                    [_string(value.get("reason")) for value in identities if _string(value.get("reason"))]
                    + list(work_support.get("validation_reasons", []))
                    + list(work_support.get("raw_static_reasons", []))
                    + ([_string(work_support.get("reason"))] if _string(work_support.get("reason")) else [])
                )),
            }
        )

    # A certificate is owned by the relevant executing invocation.  Relay-site
    # records retain their static helper function, so function-based grouping
    # would lose synchronous-helper pairs.  Module + parent is the runtime
    # validation group used by the Native validator.
    grouped: MutableMapping[Tuple[int, int, str], List[Mapping[str, Any]]] = collections.defaultdict(list)
    for emission in emissions:
        grouped[
            (
                int(emission.get("root_trace_tx_id", 0)),
                int(emission.get("parent_trace_tx_id", 0)),
                _string(emission.get("source_module_id")),
            )
        ].append(emission)
    observed_pairs = 0
    distinct_site_pairs = 0
    certified_pairs = 0
    pair_details: List[Mapping[str, Any]] = []
    pair_kind = {
        "MustPrecedeAB": "certificate_must_precede",
        "MustPrecedeBA": "certificate_must_precede",
        "CoEmissionIndependent": "certificate_coemission_independence",
    }

    def pair_certificate_evidence_valid(
        value: Mapping[str, Any], function_id: str
    ) -> bool:
        if not certificate_evidence_valid(value, function_id):
            return False
        relation = _string(value.get("relation"))
        if relation in ("MustPrecedeAB", "MustPrecedeBA"):
            return True
        referenced = [
            solver_results[item]
            for item in certificate_evidence(value)["solver_result_ids"]
            if item in solver_results
        ]

        def has(kind: str, status: str) -> bool:
            return any(
                _string(item.get("kind")) == kind
                and _string(item.get("solver_result", {}).get("status")) == status
                for item in referenced
                if isinstance(item.get("solver_result", {}), Mapping)
            )

        return relation == "CoEmissionIndependent" and has(
            "RelayMutualExclusion", "Disproved"
        ) and has("RelayTargetIndependence", "Proved")
    for (root_id, parent_id, module_id), group in sorted(grouped.items(), key=lambda item: str(item[0])):
        parent_execution = execution_by_id.get(parent_id, {})
        owner_function = (
            _string(parent_execution.get("source_function_id"))
            if _string(parent_execution.get("module_id")) == module_id
            else ""
        )
        certificate = certificates.get(owner_function, {}) if module_id == manifest_module_id else {}
        static_pairs: MutableMapping[frozenset, List[Mapping[str, Any]]] = collections.defaultdict(list)
        for pair in _mapping_list(certificate.get("pair_relations", [])):
            key = frozenset((_string(pair.get("site_a")), _string(pair.get("site_b"))))
            if len(key) == 2:
                static_pairs[key].append(pair)
        ordered = sorted(group, key=lambda item: int(item.get("emission_sequence", 0)))
        for left, right in itertools.combinations(ordered, 2):
            observed_pairs += 1
            left_id, right_id = _string(left.get("relay_site_id")), _string(right.get("relay_site_id"))
            if left_id != right_id:
                distinct_site_pairs += 1
            static_pair_records = static_pairs.get(frozenset((left_id, right_id)), []) if left_id != right_id else []
            candidate_records = [
                value for value in static_pair_records
                if _string(value.get("status")) == "Proved"
                and _string(value.get("relation")) in pair_kind
                and _string(value.get("certificate_id") or value.get("id"))
                and pair_certificate_evidence_valid(value, owner_function)
            ]
            passed_records: List[Mapping[str, Any]] = []
            pair_checks: List[Mapping[str, Any]] = []
            for record in candidate_records:
                relation = _string(record.get("relation"))
                certificate_id = _string(record.get("certificate_id") or record.get("id"))
                values = matching_validations(
                    pair_kind[relation], root_id, parent_id, module_id,
                    function_id=owner_function,
                    certificate_id=certificate_id,
                )
                values = [
                    value for value in values
                    if frozenset((
                        _string(value.get("detail", {}).get("site_a")),
                        _string(value.get("detail", {}).get("site_b")),
                    )) == frozenset((left_id, right_id))
                    and _string(value.get("detail", {}).get("certificate_relation")) == relation
                ]
                pair_checks.extend(values)
                if validation_status(values) == "Passed":
                    passed_records.append(record)
            covered = left_id != right_id and bool(passed_records)
            certified_pairs += int(covered)
            if covered:
                reason = ""
            elif left_id == right_id:
                reason = "same-site occurrence pair has no occurrence-indexed certificate"
            elif not owner_function or module_id != manifest_module_id:
                reason = "certificate owner or manifest module is unavailable"
            elif not candidate_records:
                reason = "no evidence-valid proved static pair certificate"
            else:
                reason = "no exact Passed runtime validation for a proved pair certificate"
            pair_details.append(
                {
                    "root_trace_tx_id": root_id,
                    "parent_trace_tx_id": parent_id,
                    "module_id": module_id,
                    "certificate_owner_function_id": owner_function,
                    "site_a": left_id,
                    "site_b": right_id,
                    "candidate_certificate_ids": [_string(value.get("certificate_id") or value.get("id")) for value in candidate_records],
                    "certificate_ids": [_string(value.get("certificate_id") or value.get("id")) for value in passed_records],
                    "certificate_evidence": [certificate_evidence(value) for value in passed_records],
                    "static_certificate_records": [
                        {
                            "certificate_id": _string(value.get("certificate_id") or value.get("id")),
                            "status": _string(value.get("status")),
                            "relation": _string(value.get("relation")),
                            "reason": _string(value.get("reason")),
                            "evidence_valid": pair_certificate_evidence_valid(value, owner_function),
                        }
                        for value in static_pair_records
                    ],
                    "covered": covered,
                    "validation_check_ids": list(dict.fromkeys(_string(value.get("detail", {}).get("property_id")) for value in pair_checks if _string(value.get("detail", {}).get("property_id")))),
                    "validation_reasons": list(dict.fromkeys(_string(value.get("reason")) for value in pair_checks if _string(value.get("reason")))),
                    "reason": reason,
                }
            )

    artifact_validations: MutableMapping[str, List[Mapping[str, Any]]] = collections.defaultdict(list)
    for value in validations:
        if _string(value.get("check_kind")) != "artifact_binding":
            continue
        detail = value.get("detail", {})
        if isinstance(detail, Mapping):
            artifact_validations[_string(detail.get("module_identity"))].append(value)

    def binding_status(module_id: str) -> str:
        if not module_id or module_id != manifest_module_id:
            return "ForeignOrUnboundModule"
        values = artifact_validations.get(module_id, [])
        status = validation_status(values)
        if status != "Passed":
            return status
        # The strict validator has already compared the complete artifact
        # binding (including the manifest hash).  Join that result to this
        # analysis by the compiler-bound module identity; do not recompute or
        # reinterpret the validator's hash protocol here.
        for value in values:
            identity = value.get("detail", {}).get("manifest_identity", {})
            if not isinstance(identity, Mapping) or _string(identity.get("module_id")) != module_id:
                return "ManifestBindingMismatch"
        return "Passed"

    instrumentation_counter = instrumentation_failures

    def root_instrumentation_errors(root_id: int) -> List[str]:
        reasons: List[str] = []
        if instrumentation_counter:
            reasons.append("trace counter reports instrumentation failures")
        for value in validations:
            if _string(value.get("status")) != "TraceInstrumentationError" and _string(value.get("check_kind")) != "instrumentation":
                continue
            detail = value.get("detail", {})
            detail_root = int(detail.get("root_trace_tx_id", 0)) if isinstance(detail, Mapping) else 0
            if detail_root in (0, root_id):
                reasons.append(_string(value.get("reason")) or "trace instrumentation failure")
        return list(dict.fromkeys(reasons))

    emissions_by_root: MutableMapping[int, List[Mapping[str, Any]]] = collections.defaultdict(list)
    routes_by_root: MutableMapping[int, List[Mapping[str, Any]]] = collections.defaultdict(list)
    executions_by_root: MutableMapping[int, List[Mapping[str, Any]]] = collections.defaultdict(list)
    for value in emissions:
        emissions_by_root[int(value.get("root_trace_tx_id", 0))].append(value)
    for value in routes:
        routes_by_root[int(value.get("root_trace_tx_id", 0))].append(value)
    for value in executions:
        root_id = int(value.get("root_trace_tx_id", 0))
        if root_id in selected_root_ids:
            executions_by_root[root_id].append(value)

    def route_matches_emission(route: Mapping[str, Any], emission: Mapping[str, Any]) -> bool:
        return (
            int(route.get("root_trace_tx_id", 0)) == int(emission.get("root_trace_tx_id", 0))
            and int(route.get("parent_trace_tx_id", 0)) == int(emission.get("parent_trace_tx_id", 0))
            and _string(route.get("source_module_id")) == _string(emission.get("source_module_id"))
            and int(route.get("relay_site_ordinal", -1)) == int(emission.get("relay_site_ordinal", -2))
            and int(route.get("occurrence_index", -1)) == int(emission.get("occurrence_index", -2))
            and _string(route.get("relay_site_id")) == _string(emission.get("relay_site_id"))
        )

    def execution_integrity(root_id: int) -> Tuple[bool, List[str]]:
        descendants = executions_by_root.get(root_id, [])
        root_emissions = emissions_by_root.get(root_id, [])
        root_routes = routes_by_root.get(root_id, [])
        errors: List[str] = []
        ids = {int(value.get("trace_tx_id", 0)) for value in descendants}
        if not descendants:
            errors.append("root has no execution records")
        if root_id in duplicate_root_ids or any(value in duplicate_execution_ids for value in ids):
            errors.append("duplicate execution trace ID")
        if any(
            not bool(value.get("started"))
            or not bool(value.get("completed"))
            or not bool(value.get("succeeded"))
            for value in descendants
        ):
            errors.append("execution is incomplete or failed")
        if any(
            bool(value.get("is_relay")) and int(value.get("parent_trace_tx_id", 0)) not in ids
            for value in descendants
        ):
            errors.append("relay execution has no parent execution")
        for emission in root_emissions:
            if int(emission.get("parent_trace_tx_id", 0)) not in ids:
                errors.append("relay emission has no parent execution")
            child_id = int(emission.get("child_trace_tx_id", 0))
            if child_id and child_id not in ids:
                errors.append("relay emission child execution is missing")
            matching_routes = [route for route in root_routes if route_matches_emission(route, emission)]
            if not matching_routes:
                errors.append("relay emission has no physical route")
            expected_routes = int(emission.get("physical_clone_count", 0)) or 1
            if len(matching_routes) != expected_routes:
                errors.append("relay emission physical route count does not match physical_clone_count")
        for route in root_routes:
            if int(route.get("parent_trace_tx_id", 0)) not in ids:
                errors.append("physical route has no parent execution")
            physical_id = int(route.get("physical_trace_tx_id", 0))
            if physical_id and physical_id not in ids:
                errors.append("physical route execution is missing")
            if not any(route_matches_emission(route, emission) for emission in root_emissions):
                errors.append("physical route has no matching logical emission")
        routed_execution_ids = {
            int(route.get("physical_trace_tx_id", 0))
            for route in root_routes
            if int(route.get("physical_trace_tx_id", 0))
        }
        if any(
            bool(value.get("is_relay"))
            and int(value.get("trace_tx_id", 0)) not in routed_execution_ids
            for value in descendants
        ):
            errors.append("relay execution has no matching physical route")
        return not errors, list(dict.fromkeys(errors))

    def modules_used_by_root(root_id: int) -> Set[str]:
        result = {
            _string(value.get("module_id"))
            for value in executions_by_root.get(root_id, [])
            if _string(value.get("module_id"))
        }
        result.update(
            _string(value.get("source_module_id"))
            for value in emissions_by_root.get(root_id, [])
            if _string(value.get("source_module_id"))
        )
        return result

    def root_mismatches(root_id: int) -> List[str]:
        return list(dict.fromkeys(
            _string(value.get("reason")) or _string(value.get("check_kind"))
            for value in validations
            if _string(value.get("status")) == "Mismatch"
            and isinstance(value.get("detail", {}), Mapping)
            and int(value.get("detail", {}).get("root_trace_tx_id", 0)) == root_id
        ))

    def root_health(root_id: int) -> Mapping[str, Any]:
        integrity, integrity_errors = execution_integrity(root_id)
        statuses = {module_id: binding_status(module_id) for module_id in sorted(modules_used_by_root(root_id))}
        instrumentation_errors = root_instrumentation_errors(root_id)
        mismatches = root_mismatches(root_id)
        binding_healthy = bool(statuses) and all(status == "Passed" for status in statuses.values())
        return {
            "healthy_for_bounds": binding_healthy and integrity and not instrumentation_errors,
            "healthy": binding_healthy and integrity and not instrumentation_errors and not mismatches,
            "binding_statuses": statuses,
            "execution_integrity_errors": integrity_errors,
            "instrumentation_errors": instrumentation_errors,
            "mismatch_reasons": mismatches,
        }

    health_by_root = {root_id: root_health(root_id) for root_id in roots}

    # A locally matching validation row is not usable when the enclosing trace
    # tree is unbound, incomplete, or instrumentation-corrupt.  Retain the
    # local result for diagnostics, but gate every runtime-weighted numerator
    # on the root's trusted trace envelope.
    for detail in emission_details:
        root_id = int(detail.get("root_trace_tx_id", 0))
        detail["validation_covered"] = bool(detail.get("covered"))
        detail["trace_trusted"] = bool(health_by_root.get(root_id, {}).get("healthy"))
        detail["identity_trusted"] = bool(
            detail.get("identity_status") == "Passed" and detail["trace_trusted"]
        )
        detail["covered"] = bool(detail["validation_covered"] and detail["trace_trusted"])
    emission_covered = sum(bool(detail.get("covered")) for detail in emission_details)
    for detail in pair_details:
        root_id = int(detail.get("root_trace_tx_id", 0))
        detail["validation_covered"] = bool(detail.get("covered"))
        detail["trace_trusted"] = bool(health_by_root.get(root_id, {}).get("healthy"))
        detail["covered"] = bool(detail["validation_covered"] and detail["trace_trusted"])
    certified_pairs = sum(bool(detail.get("covered")) for detail in pair_details)

    def root_bound_results(root_id: int, item: Mapping[str, Any]) -> Mapping[str, Tuple[str, Mapping[str, Any]]]:
        function_id = _string(item.get("source_function_id"))
        module_id = _string(item.get("module_id"))
        return {
            field: checked_bound(function_id, field, root_id, root_id, module_id)
            for field in (
                "direct_logical_work",
                "transitive_logical_work",
                "physical_route_work",
                "relay_tree_depth",
            )
        }

    bounds_by_root = {root_id: root_bound_results(root_id, item) for root_id, item in roots.items()}
    nonempty_root_ids = {int(item.get("root_trace_tx_id", 0)) for item in emissions}
    tree_roots = {root_id: item for root_id, item in roots.items() if root_id in nonempty_root_ids}
    tree_work = 0
    tree_depth = 0
    tree_details: List[Mapping[str, Any]] = []
    for root_id, item in sorted(tree_roots.items()):
        function_id = _string(item.get("source_function_id"))
        work_status, work_support = bounds_by_root[root_id]["transitive_logical_work"]
        depth_status, depth_support = bounds_by_root[root_id]["relay_tree_depth"]
        trusted = bool(health_by_root[root_id]["healthy"])
        tree_work += int(trusted and work_status == "Passed")
        tree_depth += int(trusted and depth_status == "Passed")
        tree_details.append(
            {
                "root_trace_tx_id": root_id,
                "source_function_id": function_id,
                "transitive_work_status": work_status,
                "depth_status": depth_status,
                "trace_trusted": trusted,
                "transitive_work_certificate_id": work_support.get("certificate_id", ""),
                "transitive_work_evidence": work_support.get("evidence", {}),
                "depth_certificate_id": depth_support.get("certificate_id", ""),
                "depth_evidence": depth_support.get("evidence", {}),
            }
        )

    transactions = {
        "source_transactions": len(root_candidates),
        "complete": 0,
        "partial": 0,
        "fallback": len(duplicate_root_records),
    }
    transaction_details: List[Mapping[str, Any]] = [
        {
            "root_trace_tx_id": int(item.get("trace_tx_id", 0)),
            "source_function_id": _string(item.get("source_function_id")),
            "classification": "Fallback",
            "bound_statuses": {},
            "bound_details": {},
            "all_emissions_covered": False,
            "all_pairs_covered": False,
            "binding_statuses": {},
            "execution_integrity_errors": ["duplicate execution trace ID"],
            "instrumentation_errors": [],
            "mismatch_reasons": [],
            "reasons": ["execution_incomplete_or_failed"],
        }
        for item in duplicate_root_records
    ]
    for root_id, item in roots.items():
        function_id = _string(item.get("source_function_id"))
        related_pair_details = [detail for detail in pair_details if int(detail.get("root_trace_tx_id", 0)) == root_id]
        related_emissions = [detail for detail in emission_details if int(detail.get("root_trace_tx_id", 0)) == root_id]
        all_pairs = all(detail["covered"] for detail in related_pair_details)
        all_emissions = all(detail["covered"] for detail in related_emissions)
        bound_fields = (
            "direct_logical_work",
            "transitive_logical_work",
            "physical_route_work",
            "relay_tree_depth",
        )
        required_statuses = [bounds_by_root[root_id][field][0] for field in bound_fields]
        health = health_by_root[root_id]
        healthy = bool(health["healthy"])
        complete = healthy and all(value == "Passed" for value in required_statuses) and all_emissions and all_pairs
        any_coverage = any(value == "Passed" for value in required_statuses) or any(detail["covered"] for detail in related_pair_details) or any(detail["covered"] for detail in related_emissions)
        if complete:
            transactions["complete"] += 1
            classification = "Complete"
            fallback_reasons: List[str] = []
        elif healthy and any_coverage:
            transactions["partial"] += 1
            classification = "Partial"
            fallback_reasons = [
                kind + ":" + status
                for kind, status in zip(("direct_work", "transitive_work", "physical_work", "depth"), required_statuses)
                if status != "Passed"
            ]
            if not all_emissions:
                fallback_reasons.append("emission_certificate_incomplete")
            if not all_pairs:
                fallback_reasons.append("pair_certificate_incomplete")
        else:
            transactions["fallback"] += 1
            classification = "Fallback"
            fallback_reasons = []
            if not health["binding_statuses"] or any(value != "Passed" for value in health["binding_statuses"].values()):
                fallback_reasons.append("manifest_binding_untrusted")
            if health["instrumentation_errors"]:
                fallback_reasons.append("trace_instrumentation_failure")
            if health["execution_integrity_errors"]:
                fallback_reasons.append("execution_incomplete_or_failed")
            if health["mismatch_reasons"]:
                fallback_reasons.append("runtime_certificate_mismatch")
            if not any_coverage:
                fallback_reasons.append("no_usable_certificate_dimension")
        transaction_details.append(
            {
                "root_trace_tx_id": root_id,
                "source_function_id": function_id,
                "classification": classification,
                "bound_statuses": dict(zip(("direct_work", "transitive_work", "physical_work", "depth"), required_statuses)),
                "bound_details": {
                    label: bounds_by_root[root_id][field][1]
                    for label, field in zip(
                        ("direct_work", "transitive_work", "physical_work", "depth"),
                        bound_fields,
                    )
                },
                "all_emissions_covered": all_emissions,
                "all_pairs_covered": all_pairs,
                "binding_statuses": health["binding_statuses"],
                "execution_integrity_errors": health["execution_integrity_errors"],
                "instrumentation_errors": health["instrumentation_errors"],
                "mismatch_reasons": health["mismatch_reasons"],
                "reasons": fallback_reasons,
            }
        )

    return {
        "available": True,
        "reason": "",
        "relay_emissions": {
            "observed": len(emissions),
            "manifest_identity_validation_passed": sum(detail["identity_status"] == "Passed" for detail in emission_details),
            "manifest_identity_covered": sum(bool(detail.get("identity_trusted")) for detail in emission_details),
            "certificate_covered": emission_covered,
            "percentage": _percent(emission_covered, len(emissions)),
            "details": emission_details,
        },
        "relay_pairs": {
            "observed": observed_pairs,
            "observed_distinct_site": distinct_site_pairs,
            "certified": certified_pairs,
            # Same-site loop occurrence pairs are outside the current static
            # certificate model and are retained separately. The principal
            # pair percentage therefore uses the eligible distinct-site set.
            "percentage": _percent(certified_pairs, distinct_site_pairs),
            "all_pair_instance_percentage": _percent(certified_pairs, observed_pairs),
            "details": pair_details,
        },
        "relay_trees": {
            "executed": len(tree_roots),
            "finite_work_bound": tree_work,
            "finite_depth_bound": tree_depth,
            "work_percentage": _percent(tree_work, len(tree_roots)),
            "depth_percentage": _percent(tree_depth, len(tree_roots)),
            "details": tree_details,
        },
        "transactions": {**transactions, "details": transaction_details},
    }


def analyze_manifest(
    benchmark: str,
    manifest: Mapping[str, Any],
    trace: Optional[Mapping[str, Any]] = None,
    function_filters: Sequence[str] = (),
    source_label: str = "",
) -> Mapping[str, Any]:
    validate_manifest(manifest)
    selected = _selected_function_ids(manifest, function_filters)
    cfg = manifest.get("control_flow", {})
    cfg_functions = [
        function
        for function in _mapping_list(cfg.get("functions", []))
        if _string(function.get("function_id")) in selected
    ]
    sites = [
        site
        for site in _mapping_list(manifest.get("relay_sites", []))
        if _string(site.get("source_function_id")) in selected
    ]
    selected_site_ids = {_string(site.get("id")) for site in sites}
    protocol_functions = [
        function
        for function in _mapping_list(manifest.get("functions", []))
        if _string(function.get("source_function_id")) in selected
    ]
    protocol_emitters = {
        _string(function.get("source_function_id"))
        for function in protocol_functions
        if any(_string(site_id) in selected_site_ids for site_id in function.get("relay_site_ids", []))
    }
    call_edges = _mapping_list(cfg.get("synchronous_call_graph", {}).get("edges", []))
    sync_edges = [
        edge
        for edge in call_edges
        if _string(edge.get("kind")) in ("Synchronous", "ExternalUnknown") and _string(edge.get("caller")) in selected
    ]
    icfg_analyses = [
        analysis
        for analysis in _mapping_list(cfg.get("relay_icfg", {}).get("function_analyses", []))
        if _string(analysis.get("function_id")) in selected
    ]
    icfg_compositions = [
        analysis
        for analysis in _mapping_list(cfg.get("relay_icfg", {}).get("synchronous_composition_analyses", []))
        if _string(analysis.get("root_function_id")) in selected
    ]

    handlers = {_string(item.get("id")): item for item in _mapping_list(manifest.get("handlers", []))}
    edges = [
        edge
        for edge in _mapping_list(manifest.get("edges", []))
        if _string(edge.get("source_function_id")) in selected
    ]
    covered_site_ids: Set[str] = set()
    for function in protocol_functions:
        covered_site_ids.update(
            _string(site_id)
            for site_id in function.get("relay_site_ids", [])
            if _string(site_id) in selected_site_ids
        )
    site_id_counts = collections.Counter(_string(site.get("id")) for site in sites)
    cfg_emit_counts = collections.Counter(
        _string(node.get("relay_site_id"))
        for function in cfg_functions
        for node in _mapping_list(function.get("nodes", []))
        if _string(node.get("kind")) == "RelayEmit"
    )
    edge_counts = collections.Counter(_string(edge.get("relay_site_id")) for edge in edges)
    cfg_function_ids = {_string(function.get("function_id")) for function in cfg_functions}
    fully_linked_site_ids: List[str] = []
    resolved_handler_site_ids: List[str] = []
    for site in sites:
        site_id = _string(site.get("id"))
        matching_edges = [edge for edge in edges if _string(edge.get("relay_site_id")) == site_id]
        if len(matching_edges) != 1:
            continue
        edge = matching_edges[0]
        handler = handlers.get(_string(edge.get("handler_id")), {})
        if (
            bool(edge.get("resolved"))
            and bool(handler.get("resolved"))
            and _string(handler.get("target_function_id")) in cfg_function_ids
        ):
            resolved_handler_site_ids.append(site_id)
        if (
            site_id_counts[site_id] == 1
            and cfg_emit_counts[site_id] == 1
            and edge_counts[site_id] == 1
            and bool(edge.get("resolved"))
            and bool(handler.get("resolved"))
            and _string(handler.get("target_function_id")) in cfg_function_ids
        ):
            fully_linked_site_ids.append(site_id)

    certificates = {
        _string(item.get("source_function_id")): item
        for item in _mapping_list(manifest.get("parallel_certificate", {}).get("functions", []))
        if _string(item.get("source_function_id")) in selected
    }
    static_cfg_ids = _structured_ids(manifest.get("control_flow", {}))
    static_constraint_ids = {
        _string(value.get("id"))
        for value in _mapping_list(manifest.get("refinement", {}).get("constraints", []))
        if _string(value.get("id"))
    }
    static_solver_ids = {
        _string(value.get("id"))
        for value in _mapping_list(manifest.get("refinement", {}).get("proof_obligations", []))
        if _string(value.get("id"))
    }

    def bound_is_covered(function_id: str, value: Any, field: str) -> bool:
        if not isinstance(value, Mapping):
            return False
        if _string(value.get("status")) not in ("Proved", "Complete", "Conservative"):
            return False
        if not _string(value.get("certificate_id") or value.get("id")):
            return False
        finite = (
            _string(value.get("bound_kind")) in ("Constant", "ParameterizedUpperBound")
            if field == "physical_route_work"
            else _expr_finite(value.get("upper_bound"))
        )
        if not finite:
            return False
        evidence = _evidence(value)
        return (
            any(evidence.values())
            and function_id in evidence["cfg_fact_ids"]
            and all(item in static_cfg_ids for item in evidence["cfg_fact_ids"])
            and all(item in static_constraint_ids for item in evidence["constraint_ids"])
            and all(item in static_solver_ids for item in evidence["solver_result_ids"])
        )

    def count_finite(field: str, alias: str = "", function_ids: Optional[Iterable[str]] = None) -> Tuple[int, List[str]]:
        evidence_ids: List[str] = []
        for function_id in sorted(protocol_emitters if function_ids is None else function_ids):
            certificate = certificates.get(function_id, {})
            value = certificate.get(field, certificate.get(alias, {}))
            if bound_is_covered(function_id, value, field):
                evidence_ids.append(_string(value.get("certificate_id") or value.get("id")))
        return len(evidence_ids), evidence_ids

    direct_count, direct_ids = count_finite("direct_logical_work", "direct_work_bound")
    transitive_count, transitive_ids = count_finite("transitive_logical_work", "transitive_work_bound")
    depth_count, depth_ids = count_finite("relay_tree_depth", "depth_bound")
    physical_ids: List[str] = []
    for function_id in sorted(protocol_emitters):
        value = certificates.get(function_id, {}).get("physical_route_work", {})
        if bound_is_covered(function_id, value, "physical_route_work"):
            physical_ids.append(_string(value.get("certificate_id") or value.get("id")))
    all_direct_count, all_direct_ids = count_finite("direct_logical_work", "direct_work_bound", selected)
    all_transitive_count, all_transitive_ids = count_finite("transitive_logical_work", "transitive_work_bound", selected)
    all_depth_count, all_depth_ids = count_finite("relay_tree_depth", "depth_bound", selected)
    all_physical_ids: List[str] = []
    for function_id in sorted(selected):
        value = certificates.get(function_id, {}).get("physical_route_work", {})
        if bound_is_covered(function_id, value, "physical_route_work"):
            all_physical_ids.append(_string(value.get("certificate_id") or value.get("id")))

    obligations = [
        obligation
        for obligation in _mapping_list(manifest.get("refinement", {}).get("proof_obligations", []))
        if not _string(obligation.get("source_function_id")) or _string(obligation.get("source_function_id")) in selected
    ]
    by_kind: MutableMapping[str, List[Mapping[str, Any]]] = collections.defaultdict(list)
    for obligation in obligations:
        by_kind[_string(obligation.get("kind"))].append(obligation)

    all_target_obligations = [item for item in by_kind["RelayTargetEquality"] if _string(item.get("relay_site_id")) in selected_site_ids]
    target_obligations = list(all_target_obligations)
    argument_obligations = [item for item in by_kind["RelayArgumentEquality"] if _string(item.get("relay_site_id")) in selected_site_ids]
    guard_obligations = [item for item in by_kind["RelayGuardNecessity"] if _string(item.get("relay_site_id")) in selected_site_ids]
    guard_equivalence_obligations = [item for item in by_kind["RelayGuardEquivalence"] if _string(item.get("relay_site_id")) in selected_site_ids]
    count_obligations = [item for item in by_kind["RelayCountEquality"] if _string(item.get("source_function_id")) in selected]
    count_upper_obligations = [item for item in by_kind["RelayCountUpperBound"] if _string(item.get("source_function_id")) in selected]
    depth_obligations = [
        item
        for kind, values in by_kind.items()
        if "Depth" in kind
        for item in values
        if _string(item.get("source_function_id")) in selected
    ]
    eligible_arguments = sum(len(_mapping_list(site.get("arguments", []))) for site in sites)
    eligible_guards = sum(1 for site in sites if _mapping_list(site.get("branches", [])))
    eligible_targets = sum(_string(site.get("relay_kind")) == "custom_scope" for site in sites)
    target_obligations = [
        item
        for item in target_obligations
        if any(
            _string(site.get("id")) == _string(item.get("relay_site_id"))
            and _string(site.get("relay_kind")) == "custom_scope"
            for site in sites
        )
    ]

    certificate_records = _certificate_records(manifest, selected, protocol_emitters)
    constraints = [
        value
        for value in _mapping_list(manifest.get("refinement", {}).get("constraints", []))
        if not _string(value.get("source_function_id")) or _string(value.get("source_function_id")) in selected
    ]
    nonnegative_constraints = [value for value in constraints if _string(value.get("kind")) == "RelayCountNonNegative"]
    refinement_symbol_sorts = {
        _string(value.get("id")): value.get("sort")
        for value in _mapping_list(manifest.get("refinement", {}).get("symbols", []))
        if _string(value.get("id"))
    }

    def supported_entity_ids(
        values: Iterable[Mapping[str, Any]],
        key_builder: Any,
        eligible: Set[Any],
    ) -> Tuple[int, List[str]]:
        by_entity: Dict[Any, str] = {}
        for value in values:
            key = key_builder(value)
            if key in eligible and _obligation_is_supported(value, refinement_symbol_sorts):
                by_entity.setdefault(key, _string(value.get("id")))
        return len(by_entity), [by_entity[key] for key in sorted(by_entity, key=repr)]

    eligible_site_keys = set(selected_site_ids)
    custom_target_keys = {
        _string(site.get("id")) for site in sites
        if _string(site.get("relay_kind")) == "custom_scope"
    }
    nontrivial_guard_keys = {
        _string(site.get("id")) for site in sites if _mapping_list(site.get("branches", []))
    }
    eligible_argument_keys = {
        (_string(site.get("id")), index)
        for site in sites
        for index, _ in enumerate(_mapping_list(site.get("arguments", [])))
    }
    eligible_function_keys = {_string(function.get("function_id")) for function in cfg_functions}
    all_target_covered = supported_entity_ids(
        all_target_obligations, lambda value: _string(value.get("relay_site_id")), eligible_site_keys
    )
    custom_target_covered = supported_entity_ids(
        target_obligations, lambda value: _string(value.get("relay_site_id")), custom_target_keys
    )
    argument_covered = supported_entity_ids(
        argument_obligations,
        lambda value: (_string(value.get("relay_site_id")), value.get("argument_index")),
        eligible_argument_keys,
    )
    guard_covered = supported_entity_ids(
        guard_obligations, lambda value: _string(value.get("relay_site_id")), eligible_site_keys
    )
    nontrivial_guard_covered = supported_entity_ids(
        guard_obligations, lambda value: _string(value.get("relay_site_id")), nontrivial_guard_keys
    )
    guard_equivalence_covered = supported_entity_ids(
        guard_equivalence_obligations, lambda value: _string(value.get("relay_site_id")), eligible_site_keys
    )
    count_covered = supported_entity_ids(
        count_obligations, lambda value: _string(value.get("source_function_id")), eligible_function_keys
    )
    count_upper_covered = supported_entity_ids(
        count_upper_obligations, lambda value: _string(value.get("source_function_id")), eligible_function_keys
    )
    depth_covered = supported_entity_ids(
        depth_obligations, lambda value: _string(value.get("source_function_id")), eligible_function_keys
    )
    nonnegative_by_function = {
        _string(value.get("source_function_id")): _string(value.get("id"))
        for value in nonnegative_constraints
        if _string(value.get("source_function_id")) in eligible_function_keys
    }
    quality: Dict[str, Dict[str, int]] = {
        prop: {status: 0 for status in REQUESTED_STATUSES} for prop in REQUESTED_PROPERTIES
    }
    negative_counts: Dict[str, int] = {prop: 0 for prop in REQUESTED_PROPERTIES}
    detailed_outcomes: Dict[str, Dict[str, int]] = {prop: {} for prop in REQUESTED_PROPERTIES}
    unknown_reasons: collections.Counter = collections.Counter()
    unknown_reason_multilabel: collections.Counter = collections.Counter()
    for record in certificate_records:
        prop, status = _string(record.get("property")), _string(record.get("status"))
        detailed_outcomes[prop][status] = detailed_outcomes[prop].get(status, 0) + 1
        projected = status
        if status in ("NotEstablished", "InvalidEvidence"):
            projected = "Unknown"
        if projected in REQUESTED_STATUSES:
            quality[prop][projected] += 1
            if projected in ("Unknown", "Unsupported"):
                unknown_reasons[_string(record.get("unknown_reason")) or "other_conservative"] += 1
                for category in record.get("reason_categories", []) or ["other_conservative"]:
                    unknown_reason_multilabel[_string(category)] += 1
        elif status == "Disproved":
            negative_counts[prop] += 1

    fanout_kinds = collections.Counter(_string(site.get("relay_kind")) for site in sites)
    fanout_ids = [_string(site.get("id")) for site in sites if _string(site.get("relay_kind"))]
    result = {
        "benchmark": benchmark,
        "source": source_label,
        "contract": _string(manifest.get("contract")),
        "schema_version": manifest.get("schema_version"),
        "analysis_scope": "function_slice" if function_filters else "whole_contract",
        "selected_function_ids": sorted(selected),
        "program_coverage": {
            "contracts": 1,
            "functions": len(cfg_functions),
            "source_functions": sum(not bool(function.get("generated_relay_lambda")) for function in cfg_functions),
            "generated_relay_lambda_functions": sum(bool(function.get("generated_relay_lambda")) for function in cfg_functions),
            "cfg_nodes_excluding_entry_exit": sum(1 for function in cfg_functions for node in _mapping_list(function.get("nodes", [])) if _string(node.get("kind")) not in ("Entry", "Exit")),
            "relay_sites": len(sites),
            "resolved_synchronous_calls": sum(_string(edge.get("kind")) == "Synchronous" and bool(edge.get("resolved")) for edge in sync_edges),
            "unresolved_calls": sum(_string(edge.get("kind")) == "ExternalUnknown" or not bool(edge.get("resolved")) for edge in sync_edges),
            "cfg_complete_functions": sum(_string(function.get("status")) == "Complete" for function in cfg_functions),
            "icfg_complete_functions": sum(_string(function.get("status")) == "Complete" for function in icfg_analyses),
            "cfg_analyzable_functions": sum(_string(function.get("status")) in ("Complete", "Conservative") for function in cfg_functions),
            "icfg_analyzable_functions": sum(_string(function.get("status")) in ("Complete", "Conservative") for function in icfg_analyses),
            "cfg_completeness_percentage": _percent(sum(_string(function.get("status")) == "Complete" for function in cfg_functions), len(cfg_functions)),
            "icfg_completeness_percentage": _percent(sum(_string(function.get("status")) == "Complete" for function in icfg_analyses), len(cfg_functions)),
            "cfg_analyzable_percentage": _percent(sum(_string(function.get("status")) in ("Complete", "Conservative") for function in cfg_functions), len(cfg_functions)),
            "icfg_analyzable_percentage": _percent(sum(_string(function.get("status")) in ("Complete", "Conservative") for function in icfg_analyses), len(cfg_functions)),
            "cfg_status_breakdown": dict(sorted(collections.Counter(_string(function.get("status")) for function in cfg_functions).items())),
            "icfg_status_breakdown": dict(sorted({
                **collections.Counter(_string(function.get("status")) for function in icfg_analyses),
                **({"Missing": len(cfg_functions) - len(icfg_analyses)} if len(icfg_analyses) < len(cfg_functions) else {}),
            }.items())),
            "icfg_compositions": {
                "roots": len(icfg_compositions),
                "complete": sum(_string(item.get("status")) == "Complete" for item in icfg_compositions),
            },
            "icfg_resolved_edges": {
                "synchronous": sum(_string(item.get("kind")) == "SyncCall" and bool(item.get("resolved")) for item in _mapping_list(cfg.get("relay_icfg", {}).get("interprocedural_edges", [])) if _string(item.get("source_function_id")) in selected),
                "async_handlers": sum(_string(item.get("kind")) == "AsyncRelaySpawn" and bool(item.get("resolved")) for item in _mapping_list(cfg.get("relay_icfg", {}).get("interprocedural_edges", [])) if _string(item.get("source_function_id")) in selected),
            },
        },
        "protocol_coverage": {
            "functions_with_relay_protocol": len(protocol_emitters),
            "functions_relay_reachable": sum(bool(value) for function_id, value in cfg.get("synchronous_call_graph", {}).get("analysis", {}).get("relay_reachable", {}).items() if function_id in selected),
            "relay_sites_structurally_recorded": _coverage_count(len(covered_site_ids), len(sites), sorted(covered_site_ids)),
            "relay_sites_covered": _coverage_count(len(fully_linked_site_ids), len(sites), sorted(fully_linked_site_ids)),
            "handler_relations": _coverage_count(len(resolved_handler_site_ids), len(sites), sorted(resolved_handler_site_ids)),
            "unresolved_handler_site_ids": sorted(selected_site_ids.difference(resolved_handler_site_ids)),
            "unresolved_handler_ids": sorted({_string(edge.get("handler_id")) for edge in edges if not bool(edge.get("resolved")) or not bool(handlers.get(_string(edge.get("handler_id")), {}).get("resolved"))}),
            "direct_logical_work_bound": _coverage_count(direct_count, len(protocol_emitters), direct_ids),
            "transitive_logical_work_bound": _coverage_count(transitive_count, len(protocol_emitters), transitive_ids),
            "physical_route_bound": _coverage_count(len(physical_ids), len(protocol_emitters), physical_ids),
            "relay_tree_depth_bound": _coverage_count(depth_count, len(protocol_emitters), depth_ids),
            "fanout_information": _coverage_count(len(fanout_ids), len(sites), fanout_ids),
            "fanout_kinds": dict(sorted(fanout_kinds.items())),
            "all_function_bound_coverage": {
                "denominator_note": "includes trivial zero-relay functions; relay-relevant metrics above are primary",
                "direct_logical_work_bound": _coverage_count(all_direct_count, len(selected), all_direct_ids),
                "transitive_logical_work_bound": _coverage_count(all_transitive_count, len(selected), all_transitive_ids),
                "physical_route_bound": _coverage_count(len(all_physical_ids), len(selected), all_physical_ids),
                "relay_tree_depth_bound": _coverage_count(all_depth_count, len(selected), all_depth_ids),
            },
        },
        "refinement_coverage": {
            "target_formulas": _coverage_count(all_target_covered[0], len(sites), all_target_covered[1]),
            "target_formulas_custom_scope": _coverage_count(custom_target_covered[0], eligible_targets, custom_target_covered[1]),
            "implicit_target_sites": len(sites) - eligible_targets,
            "argument_formulas": _coverage_count(argument_covered[0], eligible_arguments, argument_covered[1]),
            "guard_formulas": _coverage_count(guard_covered[0], len(sites), guard_covered[1]),
            "nontrivial_guard_formulas": _coverage_count(nontrivial_guard_covered[0], eligible_guards, nontrivial_guard_covered[1]),
            "guard_equivalence": _coverage_count(guard_equivalence_covered[0], len(sites), guard_equivalence_covered[1]),
            "count_constraints": _coverage_count(count_covered[0], len(cfg_functions), count_covered[1]),
            "count_upper_constraints": _coverage_count(count_upper_covered[0], len(cfg_functions), count_upper_covered[1]),
            "count_nonnegative_constraints": _coverage_count(len(nonnegative_by_function), len(cfg_functions), [nonnegative_by_function[key] for key in sorted(nonnegative_by_function)]),
            "depth_constraints": {
                **_coverage_count(depth_covered[0], len(cfg_functions), depth_covered[1]),
                "available_in_refinement_ir": bool(depth_obligations),
                "represented_elsewhere": "parallel_certificate.functions[].relay_tree_depth",
                "reason": "schema-v5 has no refinement depth-obligation kind" if not depth_obligations else "",
            },
        },
        "certificate_quality": {
            "counts": quality,
            "negative_results": negative_counts,
            "detailed_outcomes": detailed_outcomes,
            "unknown_reason_breakdown": {
                category: int(unknown_reasons.get(category, 0))
                for category in UNKNOWN_REASON_CATEGORIES
            },
            "unknown_reason_multilabel": {
                category: int(unknown_reason_multilabel.get(category, 0))
                for category in UNKNOWN_REASON_CATEGORIES
            },
            "records": certificate_records,
        },
        "runtime_weighted_coverage": _runtime_weighted(manifest, trace, selected),
    }
    return result


def load_json(path: pathlib.Path) -> Mapping[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, Mapping):
        raise ValueError("expected a JSON object in %s" % path)
    return value
