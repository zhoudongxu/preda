#!/usr/bin/env python3
"""Cross-version Formula IR comparison for semantic relay mutations.

The baseline and mutant expressions are independent inputs to a preservation
goal.  Neither side is asserted as an assumption, so a ``Disproved`` result is
not a circular proof of a compiler-emitted semantic definition.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import subprocess
import copy
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping, Sequence, Tuple


TARGET_KINDS = {"TargetArithmeticPerturb", "TargetVariableSwap"}
ARGUMENT_KINDS = {"ArgumentArithmeticPerturb"}
GUARD_KINDS = {"GuardBoundaryChange"}


class FormulaEncodingError(RuntimeError):
    pass


def canonical_formula(formula: Any) -> Any:
    if not isinstance(formula, Mapping):
        return None
    return {
        "kind": formula.get("kind"),
        "sort": {
            "kind": formula.get("sort", {}).get("kind"),
            "bit_width": formula.get("sort", {}).get("bit_width", 0),
        },
        "operator": formula.get("operator", ""),
        "symbol_id": formula.get("symbol_id", ""),
        "literal_value": formula.get("literal_value", ""),
        "unknown_reason": formula.get("unknown_reason", ""),
        "children": [
            canonical_formula(child) for child in formula.get("children", [])
        ],
    }


def formula_digest(formula: Mapping[str, Any]) -> str:
    encoded = json.dumps(
        canonical_formula(formula),
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _site(manifest: Mapping[str, Any], site_id: str) -> Mapping[str, Any]:
    return next(
        (
            site
            for site in manifest.get("relay_sites", [])
            if isinstance(site, Mapping) and site.get("id") == site_id
        ),
        {},
    )


def _normalize_formula_symbols(
    manifest: Mapping[str, Any],
    site_id: str,
    formula: Mapping[str, Any],
) -> Mapping[str, Any]:
    """Map compiler IDs to stable semantic roles for one aligned relay site.

    Listener ordinals and generated lambda names may change after an unrelated
    edit.  Differential queries must share the same logical input symbols,
    rather than declaring baseline and mutant spellings as independent values.
    """

    site = _site(manifest, site_id)
    current_function = str(site.get("source_function_id", ""))
    symbols = {
        str(value.get("id", "")): value
        for value in manifest.get("refinement", {}).get("symbols", [])
        if isinstance(value, Mapping) and value.get("id")
    }
    result = copy.deepcopy(formula)

    def visit(node: Any) -> None:
        if not isinstance(node, dict):
            return
        if node.get("kind") == "Symbol":
            raw_id = str(node.get("symbol_id", ""))
            metadata = symbols.get(raw_id)
            if metadata is not None:
                owner = str(metadata.get("source_function_id", ""))
                owner_role = "current_function" if owner == current_function else owner
                relay_site = str(metadata.get("relay_site_id", ""))
                site_role = "current_site" if relay_site == site_id else relay_site
                node["symbol_id"] = "::".join(
                    (
                        "semantic",
                        owner_role,
                        str(metadata.get("kind", "")),
                        str(metadata.get("source_name", "")),
                        str(metadata.get("preda_type", "")),
                        str(metadata.get("argument_index", -1)),
                        site_role,
                    )
                )
        for child in node.get("children", []):
            visit(child)

    visit(result)
    return result


def _constraint(
    manifest: Mapping[str, Any],
    site_id: str,
    kind: str,
    argument_index: int | None = None,
) -> Mapping[str, Any] | None:
    constraints = manifest.get("refinement", {}).get("constraints", [])
    for candidate in constraints:
        if not isinstance(candidate, Mapping):
            continue
        if candidate.get("relay_site_id") != site_id:
            continue
        if candidate.get("kind") != kind:
            continue
        if argument_index is not None and int(
            candidate.get("argument_index", -1)
        ) != argument_index:
            continue
        return candidate
    return None


def _relation_rhs(constraint: Mapping[str, Any] | None) -> Mapping[str, Any] | None:
    if not isinstance(constraint, Mapping):
        return None
    formula = constraint.get("formula")
    if not isinstance(formula, Mapping):
        return None
    children = formula.get("children", [])
    if formula.get("kind") == "Binary" and formula.get("operator") == "implies":
        if len(children) != 2 or not isinstance(children[1], Mapping):
            return None
        formula = children[1]
        children = formula.get("children", [])
    if formula.get("kind") != "Binary" or formula.get("operator") != "==":
        return None
    if len(children) != 2 or not isinstance(children[1], Mapping):
        return None
    return children[1]


def _guard_rhs(constraint: Mapping[str, Any] | None) -> Mapping[str, Any] | None:
    if not isinstance(constraint, Mapping):
        return None
    formula = constraint.get("formula")
    if not isinstance(formula, Mapping):
        return None
    children = formula.get("children", [])
    if formula.get("kind") == "Binary" and formula.get("operator") == "implies":
        if len(children) == 2 and isinstance(children[1], Mapping):
            return children[1]
    return None


def _true_formula() -> Mapping[str, Any]:
    return {
        "kind": "BoolLiteral",
        "sort": {"kind": "Bool"},
        "literal_value": "true",
        "children": [],
    }


def _and_formula(children: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    if not children:
        return _true_formula()
    if len(children) == 1:
        return children[0]
    return {
        "kind": "Nary",
        "sort": {"kind": "Bool"},
        "operator": "and",
        "children": list(children),
    }


def _equality_formula(
    baseline: Mapping[str, Any], mutant: Mapping[str, Any]
) -> Mapping[str, Any]:
    return {
        "kind": "Binary",
        "sort": {"kind": "Bool"},
        "operator": "==",
        "children": [baseline, mutant],
    }


def _sort_key(sort: Mapping[str, Any]) -> Tuple[str, int]:
    return str(sort.get("kind", "Unknown")), int(sort.get("bit_width", 0) or 0)


def _smt_sort(sort: Mapping[str, Any]) -> str:
    kind, width = _sort_key(sort)
    if kind == "Bool":
        return "Bool"
    if kind == "Int":
        return "Int"
    if kind == "UnsignedBitVector" and width > 0:
        return f"(_ BitVec {width})"
    if kind == "Address":
        return "Address"
    raise FormulaEncodingError(f"unsupported Formula IR sort {kind}/{width}")


def _walk(formula: Mapping[str, Any]) -> Iterable[Mapping[str, Any]]:
    yield formula
    for child in formula.get("children", []):
        if isinstance(child, Mapping):
            yield from _walk(child)


def _parse_integer(value: str, suffix_pattern: str) -> int:
    cleaned = re.sub(suffix_pattern, "", value.strip(), flags=re.IGNORECASE)
    cleaned = cleaned.replace("_", "")
    if not cleaned:
        raise FormulaEncodingError(f"empty numeric literal {value!r}")
    sign = -1 if cleaned.startswith("-") else 1
    unsigned = cleaned[1:] if cleaned[:1] in {"+", "-"} else cleaned
    base = 16 if unsigned.lower().startswith("0x") else 2 if unsigned.lower().startswith("0b") else 10
    return sign * int(unsigned, base)


@dataclass
class EncodedProblem:
    declarations: List[str]
    assumptions: str
    goal: str
    symbol_names: Mapping[str, str]


class FormulaSmtEncoder:
    def __init__(self, formulas: Sequence[Mapping[str, Any]]) -> None:
        symbol_sorts: Dict[str, Tuple[str, int]] = {}
        for formula in formulas:
            for node in _walk(formula):
                if node.get("kind") != "Symbol":
                    continue
                symbol_id = str(node.get("symbol_id", ""))
                if not symbol_id:
                    raise FormulaEncodingError("Formula IR symbol has an empty ID")
                sort_key = _sort_key(node.get("sort", {}))
                previous = symbol_sorts.setdefault(symbol_id, sort_key)
                if previous != sort_key:
                    raise FormulaEncodingError(
                        f"symbol {symbol_id!r} has conflicting sorts"
                    )
        self.symbol_sorts = symbol_sorts
        self.symbol_names = {
            symbol_id: f"s{index}"
            for index, symbol_id in enumerate(sorted(symbol_sorts))
        }

    def declarations(self) -> List[str]:
        result: List[str] = []
        if any(kind == "Address" for kind, _ in self.symbol_sorts.values()):
            result.append("(declare-sort Address 0)")
        for symbol_id in sorted(self.symbol_sorts):
            kind, width = self.symbol_sorts[symbol_id]
            result.append(
                f"(declare-fun {self.symbol_names[symbol_id]} () "
                f"{_smt_sort({'kind': kind, 'bit_width': width})})"
            )
        return result

    def encode(self, formula: Mapping[str, Any]) -> str:
        kind = str(formula.get("kind", "Unknown"))
        sort = formula.get("sort", {})
        sort_kind, width = _sort_key(sort)
        raw_children = formula.get("children", [])
        if not isinstance(raw_children, Sequence) or isinstance(
            raw_children, (str, bytes)
        ):
            raise FormulaEncodingError(f"{kind} children is not an array")
        if any(not isinstance(child, Mapping) for child in raw_children):
            raise FormulaEncodingError(f"{kind} contains a malformed child")
        children = list(raw_children)
        if kind == "Unknown" or sort_kind == "Unknown":
            reason = formula.get("unknown_reason", "unknown Formula IR node")
            raise FormulaEncodingError(str(reason))
        if kind == "BoolLiteral":
            if sort_kind != "Bool":
                raise FormulaEncodingError("Boolean literal has a non-Bool sort")
            value = str(formula.get("literal_value", "")).lower()
            if value not in {"true", "false"}:
                raise FormulaEncodingError(f"invalid Boolean literal {value!r}")
            return value
        if kind == "IntLiteral":
            if sort_kind != "Int":
                raise FormulaEncodingError("integer literal has a non-Int sort")
            return str(_parse_integer(str(formula.get("literal_value", "")), r"ib$"))
        if kind == "BitVectorLiteral":
            if width <= 0:
                raise FormulaEncodingError("bit-vector literal has no width")
            literal_text = str(formula.get("literal_value", ""))
            suffix = re.search(r"u(\d+)$", literal_text, re.IGNORECASE)
            if suffix is not None and int(suffix.group(1)) != width:
                raise FormulaEncodingError(
                    f"bit-vector literal suffix u{suffix.group(1)} disagrees with width {width}"
                )
            value = _parse_integer(
                literal_text, r"u(?:8|16|32|64|96|128|160|256|512)?$"
            )
            return f"(_ bv{value % (1 << width)} {width})"
        if kind == "AddressLiteral":
            raise FormulaEncodingError("address literals are not encoded by the differential runner")
        if kind == "Symbol":
            symbol_id = str(formula.get("symbol_id", ""))
            if symbol_id not in self.symbol_names:
                raise FormulaEncodingError(f"undeclared symbol {symbol_id!r}")
            return self.symbol_names[symbol_id]
        if kind == "Group":
            if len(children) != 1:
                raise FormulaEncodingError("Group requires one child")
            if _sort_key(children[0].get("sort", {})) != _sort_key(sort):
                raise FormulaEncodingError("Group changes its child sort")
            return self.encode(children[0])
        if kind == "Unary":
            if len(children) != 1:
                raise FormulaEncodingError("Unary requires one child")
            value = self.encode(children[0])
            op = str(formula.get("operator", ""))
            child_sort = _sort_key(children[0].get("sort", {}))
            if child_sort != _sort_key(sort):
                raise FormulaEncodingError("Unary result and operand sorts differ")
            mapping = {"!": "not", "not": "not", "~": "bvnot"}
            if op in {"!", "not"} and sort_kind != "Bool":
                raise FormulaEncodingError("Boolean negation requires Bool")
            if op == "~" and sort_kind != "UnsignedBitVector":
                raise FormulaEncodingError("bitwise negation requires a bit vector")
            if op == "+":
                return value
            if op == "-":
                return f"({'bvneg' if sort_kind == 'UnsignedBitVector' else '-'} {value})"
            if op in mapping:
                return f"({mapping[op]} {value})"
            raise FormulaEncodingError(f"unsupported unary operator {op!r}")
        if kind == "Binary":
            if len(children) != 2:
                raise FormulaEncodingError("Binary requires two children")
            left = self.encode(children[0])
            right = self.encode(children[1])
            op = str(formula.get("operator", ""))
            left_sort = _sort_key(children[0].get("sort", {}))
            right_sort = _sort_key(children[1].get("sort", {}))
            if left_sort != right_sort:
                raise FormulaEncodingError("Binary operand sorts differ")
            child_sort_kind, _ = left_sort
            if op in {"==", "="}:
                if sort_kind != "Bool":
                    raise FormulaEncodingError("equality result must be Bool")
                return f"(= {left} {right})"
            if op == "!=":
                if sort_kind != "Bool":
                    raise FormulaEncodingError("inequality result must be Bool")
                return f"(distinct {left} {right})"
            if op in {"&&", "and"}:
                if left_sort[0] != "Bool" or sort_kind != "Bool":
                    raise FormulaEncodingError("Boolean conjunction requires Bool")
                return f"(and {left} {right})"
            if op in {"||", "or"}:
                if left_sort[0] != "Bool" or sort_kind != "Bool":
                    raise FormulaEncodingError("Boolean disjunction requires Bool")
                return f"(or {left} {right})"
            if op in {"implies", "=>"}:
                if left_sort[0] != "Bool" or sort_kind != "Bool":
                    raise FormulaEncodingError("implication requires Bool")
                return f"(=> {left} {right})"
            if op == "xor" and child_sort_kind == "Bool":
                if sort_kind != "Bool":
                    raise FormulaEncodingError("Boolean xor result must be Bool")
                return f"(xor {left} {right})"
            if child_sort_kind == "UnsignedBitVector":
                mapping = {
                    "+": "bvadd", "-": "bvsub", "*": "bvmul",
                    "/": "bvudiv", "%": "bvurem", "&": "bvand",
                    "|": "bvor", "^": "bvxor", "<<": "bvshl",
                    ">>": "bvlshr", "<": "bvult", "<=": "bvule",
                    ">": "bvugt", ">=": "bvuge",
                }
            else:
                mapping = {
                    "+": "+", "-": "-", "*": "*", "/": "div",
                    "%": "mod", "<": "<", "<=": "<=", ">": ">", ">=": ">=",
                }
            if op not in mapping:
                raise FormulaEncodingError(f"unsupported binary operator {op!r}")
            comparison = op in {"<", "<=", ">", ">="}
            expected_result = ("Bool", 0) if comparison else left_sort
            if _sort_key(sort) != expected_result:
                raise FormulaEncodingError(
                    f"binary operator {op!r} has an inconsistent result sort"
                )
            return f"({mapping[op]} {left} {right})"
        if kind == "Nary":
            if not children:
                raise FormulaEncodingError("Nary requires children")
            op = str(formula.get("operator", ""))
            mapping = {"&&": "and", "and": "and", "||": "or", "or": "or", "+": "+", "*": "*"}
            if op not in mapping:
                raise FormulaEncodingError(f"unsupported n-ary operator {op!r}")
            encoded_children = [self.encode(child) for child in children]
            child_sorts = {
                _sort_key(child.get("sort", {})) for child in children
            }
            if len(child_sorts) != 1:
                raise FormulaEncodingError("Nary operand sorts differ")
            child_sort = next(iter(child_sorts))
            if op in {"&&", "and", "||", "or"}:
                if child_sort[0] != "Bool" or sort_kind != "Bool":
                    raise FormulaEncodingError("Boolean n-ary operator requires Bool")
            elif child_sort != _sort_key(sort):
                raise FormulaEncodingError("Nary arithmetic result sort differs")
            if sort_kind == "UnsignedBitVector" and op in {"+", "*"}:
                operator = "bvadd" if op == "+" else "bvmul"
                folded = encoded_children[0]
                for child in encoded_children[1:]:
                    folded = f"({operator} {folded} {child})"
                return folded
            return f"({mapping[op]} {' '.join(encoded_children)})"
        if kind == "Cast":
            if len(children) != 1:
                raise FormulaEncodingError("Cast requires one child")
            value = self.encode(children[0])
            child_kind, child_width = _sort_key(children[0].get("sort", {}))
            operator = str(formula.get("operator", ""))
            unsigned_cast = re.fullmatch(r"uint(\d+)", operator)
            if unsigned_cast is not None and (
                sort_kind != "UnsignedBitVector"
                or int(unsigned_cast.group(1)) != width
            ):
                raise FormulaEncodingError(
                    f"cast operator {operator!r} disagrees with result sort"
                )
            if child_kind == sort_kind and child_width == width:
                return value
            if child_kind == "UnsignedBitVector" and sort_kind == "UnsignedBitVector":
                if width > child_width:
                    return f"((_ zero_extend {width - child_width}) {value})"
                if width < child_width:
                    return f"((_ extract {width - 1} 0) {value})"
            if child_kind == "Int" and sort_kind == "UnsignedBitVector":
                return f"((_ int2bv {width}) {value})"
            if child_kind == "UnsignedBitVector" and sort_kind == "Int":
                return f"(bv2nat {value})"
            raise FormulaEncodingError(
                f"unsupported cast {child_kind}/{child_width} -> {sort_kind}/{width}"
            )
        if kind == "Ite":
            if len(children) != 3:
                raise FormulaEncodingError("Ite requires three children")
            if _sort_key(children[0].get("sort", {}))[0] != "Bool":
                raise FormulaEncodingError("Ite condition must be Bool")
            if (
                _sort_key(children[1].get("sort", {}))
                != _sort_key(children[2].get("sort", {}))
                or _sort_key(children[1].get("sort", {})) != _sort_key(sort)
            ):
                raise FormulaEncodingError("Ite branch/result sorts differ")
            return f"(ite {self.encode(children[0])} {self.encode(children[1])} {self.encode(children[2])})"
        if kind == "ArrayLength":
            raise FormulaEncodingError("ArrayLength has no cross-version SMT encoding")
        raise FormulaEncodingError(f"unsupported Formula IR kind {kind!r}")


def encode_problem(
    assumptions: Mapping[str, Any], goal: Mapping[str, Any]
) -> EncodedProblem:
    encoder = FormulaSmtEncoder([assumptions, goal])
    return EncodedProblem(
        encoder.declarations(),
        encoder.encode(assumptions),
        encoder.encode(goal),
        dict(encoder.symbol_names),
    )


def _run_z3(
    binary: pathlib.Path,
    declarations: Sequence[str],
    assertion: str,
    timeout_ms: int,
    values: Sequence[str] = (),
) -> Tuple[str, str, int]:
    script = [
        "(set-option :produce-models true)",
        f"(set-option :timeout {max(1, timeout_ms)})",
        *declarations,
        f"(assert {assertion})",
        "(check-sat)",
    ]
    if values:
        script.append(f"(get-value ({' '.join(values)}))")
    try:
        process = subprocess.run(
            [str(binary), "-in", "-smt2"],
            input="\n".join(script) + "\n",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=max(2.0, timeout_ms / 1000.0 + 2.0),
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return "error", str(exc), -1
    output = process.stdout.strip()
    first = output.splitlines()[0].strip() if output else ""
    return first, output if output else process.stderr.strip(), process.returncode


def solve_preservation(
    assumptions: Mapping[str, Any],
    baseline: Mapping[str, Any],
    mutant: Mapping[str, Any],
    z3_binary: pathlib.Path,
    timeout_ms: int = 1000,
) -> Mapping[str, Any]:
    if not z3_binary.is_file():
        return {"backend": "z3", "status": "NotRun", "reason": "Z3 binary not found"}
    goal = _equality_formula(baseline, mutant)

    def conjuncts(value: Mapping[str, Any]) -> Iterable[Mapping[str, Any]]:
        if value.get("kind") == "Nary" and value.get("operator") in {"and", "&&"}:
            for child in value.get("children", []):
                if isinstance(child, Mapping):
                    yield from conjuncts(child)
            return
        if (
            value.get("kind") == "Binary"
            and value.get("operator") in {"and", "&&"}
            and len(value.get("children", [])) == 2
        ):
            for child in value.get("children", []):
                if isinstance(child, Mapping):
                    yield from conjuncts(child)
            return
        yield value

    if any(canonical_formula(value) == canonical_formula(goal) for value in conjuncts(assumptions)):
        return {
            "backend": "z3",
            "status": "EncodingError",
            "reason": "circular preservation goal appears as a solver assumption",
        }
    try:
        problem = encode_problem(assumptions, goal)
    except (FormulaEncodingError, TypeError, ValueError) as exc:
        return {"backend": "z3", "status": "Unsupported", "reason": str(exc)}

    assumption_status, assumption_output, assumption_rc = _run_z3(
        z3_binary, problem.declarations, problem.assumptions, timeout_ms
    )
    if assumption_rc != 0 or assumption_status not in {"sat", "unsat", "unknown"}:
        return {
            "backend": "z3",
            "status": "EncodingError",
            "reason": assumption_output,
        }
    if assumption_status == "unsat":
        return {
            "backend": "z3",
            "status": "InconsistentAssumptions",
            "reason": "joint path assumptions are unsatisfiable",
        }
    if assumption_status == "unknown":
        return {"backend": "z3", "status": "Unknown", "reason": assumption_output}

    counterexample_assertion = f"(and {problem.assumptions} (not {problem.goal}))"
    goal_status, goal_output, goal_rc = _run_z3(
        z3_binary,
        problem.declarations,
        counterexample_assertion,
        timeout_ms,
    )
    if goal_rc != 0 or goal_status not in {"sat", "unsat", "unknown"}:
        return {"backend": "z3", "status": "EncodingError", "reason": goal_output}
    if goal_status == "unsat":
        return {"backend": "z3", "status": "Proved", "reason": "preservation goal holds"}
    if goal_status == "unknown":
        return {"backend": "z3", "status": "Unknown", "reason": goal_output}
    names = [problem.symbol_names[key] for key in sorted(problem.symbol_names)]
    projected_output = goal_output
    if names:
        projected_status, projected_output, projected_rc = _run_z3(
            z3_binary,
            problem.declarations,
            counterexample_assertion,
            timeout_ms,
            names,
        )
        if projected_rc != 0 or projected_status != "sat":
            return {
                "backend": "z3",
                "status": "EncodingError",
                "reason": projected_output,
            }
    return {
        "backend": "z3",
        "status": "Disproved",
        "reason": "Z3 found an input where baseline and mutant formulas differ",
        "projected_counterexample": projected_output,
        "symbol_map": {
            problem.symbol_names[key]: key for key in sorted(problem.symbol_names)
        },
    }


def _comparison_specs(
    mutation_type: str,
    baseline_site: str,
    mutant_site: str,
    baseline_manifest: Mapping[str, Any],
    mutant_manifest: Mapping[str, Any],
) -> Iterable[Tuple[str, int, Mapping[str, Any] | None, Mapping[str, Any] | None]]:
    if mutation_type in TARGET_KINDS:
        yield (
            "RelayTargetPreservation",
            -1,
            _relation_rhs(_constraint(baseline_manifest, baseline_site, "RelayTargetRelation")),
            _relation_rhs(_constraint(mutant_manifest, mutant_site, "RelayTargetRelation")),
        )
    elif mutation_type in ARGUMENT_KINDS:
        baseline_site_json = next(
            (site for site in baseline_manifest.get("relay_sites", []) if site.get("id") == baseline_site),
            {},
        )
        for index in range(len(baseline_site_json.get("arguments", []))):
            yield (
                "RelayArgumentPreservation",
                index,
                _relation_rhs(_constraint(baseline_manifest, baseline_site, "RelayArgumentRelation", index)),
                _relation_rhs(_constraint(mutant_manifest, mutant_site, "RelayArgumentRelation", index)),
            )
    elif mutation_type in GUARD_KINDS:
        yield (
            "RelayGuardPreservation",
            -1,
            _guard_rhs(_constraint(baseline_manifest, baseline_site, "RelayGuardNecessity")),
            _guard_rhs(_constraint(mutant_manifest, mutant_site, "RelayGuardNecessity")),
        )


def compare_manifest_refinements(
    baseline_manifest: Mapping[str, Any],
    mutant_manifest: Mapping[str, Any],
    generated: Mapping[str, Any],
    site_alignment: Mapping[str, str | None],
    z3_binary: pathlib.Path,
    timeout_ms: int = 1000,
) -> Mapping[str, Any]:
    mutation_type = str(generated.get("mutation_type", ""))
    affected = [str(value) for value in generated.get("relay_site_ids", [])]
    obligations: List[Mapping[str, Any]] = []
    for baseline_site in affected:
        mutant_site = site_alignment.get(baseline_site)
        if not mutant_site:
            obligations.append(
                {
                    "id": f"semantic.{mutation_type}.{baseline_site}.alignment",
                    "kind": "Unknown",
                    "baseline_site_id": baseline_site,
                    "mutant_site_id": "",
                    "argument_index": -1,
                    "formula_ir_changed": None,
                    "solver_result": {
                        "backend": "z3",
                        "status": "Unsupported",
                        "reason": "affected baseline relay site has no unique mutant alignment",
                    },
                }
            )
            continue
        for property_kind, argument_index, baseline, mutant in _comparison_specs(
            mutation_type,
            baseline_site,
            mutant_site,
            baseline_manifest,
            mutant_manifest,
        ):
            obligation_id = (
                f"semantic.{mutation_type}.{baseline_site}.{property_kind}."
                f"{argument_index if argument_index >= 0 else 'target'}"
            )
            if not isinstance(baseline, Mapping) or not isinstance(mutant, Mapping):
                obligations.append(
                    {
                        "id": obligation_id,
                        "kind": property_kind,
                        "baseline_site_id": baseline_site,
                        "mutant_site_id": mutant_site,
                        "argument_index": argument_index,
                        "formula_ir_changed": None,
                        "solver_result": {
                            "backend": "z3",
                            "status": "Unsupported",
                            "reason": "baseline or mutant semantic Formula IR is unavailable",
                        },
                    }
                )
                continue
            baseline = _normalize_formula_symbols(
                baseline_manifest, baseline_site, baseline
            )
            mutant = _normalize_formula_symbols(
                mutant_manifest, mutant_site, mutant
            )
            changed = canonical_formula(baseline) != canonical_formula(mutant)
            # The preservation goal compares compiler-extracted semantic
            # definitions for all inputs. Guard changes are checked by their
            # own obligation; neither the baseline nor mutant goal is assumed.
            assumptions = _true_formula()
            result = solve_preservation(
                assumptions, baseline, mutant, z3_binary, timeout_ms
            )
            obligations.append(
                {
                    "id": obligation_id,
                    "kind": property_kind,
                    "baseline_site_id": baseline_site,
                    "mutant_site_id": mutant_site,
                    "argument_index": argument_index,
                    "formula_ir_changed": changed,
                    "baseline_formula_digest": formula_digest(baseline),
                    "mutant_formula_digest": formula_digest(mutant),
                    "solver_result": result,
                }
            )
    statuses: Dict[str, int] = {}
    for obligation in obligations:
        status = str(obligation.get("solver_result", {}).get("status", "NotRun"))
        statuses[status] = statuses.get(status, 0) + 1
    return {
        "applicable": mutation_type in TARGET_KINDS | ARGUMENT_KINDS | GUARD_KINDS,
        "formula_ir_changed": any(
            obligation.get("formula_ir_changed") is True for obligation in obligations
        ),
        "statuses": statuses,
        "obligations": obligations,
        "z3_disproved": [
            str(obligation.get("id", ""))
            for obligation in obligations
            if obligation.get("solver_result", {}).get("status") == "Disproved"
        ],
        "unsupported": [
            str(obligation.get("id", ""))
            for obligation in obligations
            if obligation.get("solver_result", {}).get("status") == "Unsupported"
        ],
    }
