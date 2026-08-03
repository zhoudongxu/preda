#!/usr/bin/env python3
"""Deterministic PREDA scalability-program generator.

The generator controls source structure, not claimed analysis outcomes.  Every
run must validate the achieved CFG/protocol/constraint counts from the emitted
manifest before a sample is accepted by the experiment runner.
"""

from __future__ import annotations

import hashlib
import json
import re
from typing import Any, Dict, List, Mapping, Sequence, Tuple


class GeneratorConfigurationError(ValueError):
    pass


def _integer(config: Mapping[str, Any], key: str, default: int) -> int:
    try:
        value = int(config.get(key, default))
    except (TypeError, ValueError):
        raise GeneratorConfigurationError("%s must be an integer" % key)
    if value < 0:
        raise GeneratorConfigurationError("%s must be non-negative" % key)
    return value


def _distribute_sites(site_count: int, depth: int, width: int) -> List[int]:
    if depth == 0:
        if site_count != 0:
            raise GeneratorConfigurationError("relay_depth=0 requires relay_sites=0")
        return []
    if width <= 0:
        raise GeneratorConfigurationError("positive relay_depth requires relay_width > 0")
    if site_count < depth or site_count > depth * width:
        raise GeneratorConfigurationError(
            "relay graph requires relay_depth <= relay_sites <= relay_depth * relay_width"
        )
    counts = [1] * depth
    remaining = site_count - depth
    level = 0
    while remaining:
        capacity = width - counts[level]
        if capacity:
            delta = min(capacity, remaining)
            counts[level] += delta
            remaining -= delta
        level = (level + 1) % depth
    return counts


def _indent(lines: Sequence[str], amount: int = 1) -> List[str]:
    prefix = "    " * amount
    return [prefix + line if line else "" for line in lines]


def _wrap_control(block: List[str], branch_depth: int, loop_depth: int) -> List[str]:
    wrapped = list(block)
    for index in reversed(range(loop_depth)):
        wrapped = [
            "for (uint32 loop_%d = 0u32; loop_%d < 2u32; loop_%d++) {" % (index, index, index),
            *_indent(wrapped),
            "}",
        ]
    for index in reversed(range(branch_depth)):
        wrapped = [
            "if (selector >= %du32) {" % index,
            *_indent(wrapped),
            "}",
        ]
    return wrapped


def _source_statement_count(lines: Sequence[str]) -> int:
    """Count generated source statements without counting for-header semicolons."""
    result = 0
    for line in lines:
        stripped = line.strip()
        if stripped.endswith(";") or stripped.startswith("if (") or stripped.startswith("for ("):
            result += 1
    return result


def generate_program(config: Mapping[str, Any]) -> Tuple[str, Mapping[str, Any]]:
    requested_functions = _integer(config, "functions", 8)
    requested_statements = _integer(config, "statements", 24)
    relay_sites = _integer(config, "relay_sites", 1)
    relay_width = _integer(config, "relay_width", max(1, relay_sites))
    relay_depth = _integer(config, "relay_depth", 1 if relay_sites else 0)
    branch_depth = _integer(config, "branch_depth", 0)
    loop_depth = _integer(config, "loop_depth", 0)
    sync_call_depth = _integer(config, "sync_call_depth", 0)
    arguments_per_relay = _integer(config, "arguments_per_relay", 1)
    target_expression_terms = _integer(config, "target_expression_terms", 1)
    if target_expression_terms == 0:
        raise GeneratorConfigurationError("target_expression_terms must be positive")
    if arguments_per_relay > 32:
        raise GeneratorConfigurationError("arguments_per_relay > 32 is intentionally unsupported")

    level_sites = _distribute_sites(relay_sites, relay_depth, relay_width)
    structural_functions = 1 + relay_depth + sync_call_depth
    if requested_functions < structural_functions:
        raise GeneratorConfigurationError(
            "functions=%d is below structural minimum %d" % (requested_functions, structural_functions)
        )

    canonical = json.dumps(dict(config), sort_keys=True, separators=(",", ":"))
    suffix = hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:12]
    contract = "Scale_%s" % suffix
    argument_declarations = ["uint32 arg_%d" % index for index in range(arguments_per_relay)]
    argument_names = ["arg_%d" % index for index in range(arguments_per_relay)]
    handler_parameters = list(argument_declarations)
    handler_arguments = list(argument_names)

    functions: List[List[str]] = []
    # The last handler is the leaf. Earlier handlers form the async depth
    # chain and own the relay sites for levels 1..D-1.
    for handler_index in reversed(range(relay_depth)):
        body: List[str]
        if handler_index == relay_depth - 1:
            body = ["observed += %s;" % (argument_names[0] if argument_names else "1u32")]
        else:
            body = []
            for site_index in range(level_sites[handler_index + 1]):
                offset = 100000 + handler_index * relay_width + site_index
                base = argument_names[0] if argument_names else "0u32"
                terms = [base] + ["%du32" % (offset + term) for term in range(target_expression_terms - 1)]
                target = " + ".join(terms)
                body.append("relay@(%s) handler_%d(%s);" % (target, handler_index + 1, ", ".join(handler_arguments)))
        functions.append(
            [
                "@uint32 function handler_%d(%s) {" % (handler_index, ", ".join(handler_parameters)),
                *_indent(body),
                "}",
            ]
        )

    for helper_index in range(sync_call_depth):
        expression = "selector + 1u32" if helper_index == 0 else "sync_%d(selector)" % (helper_index - 1)
        functions.append(
            [
                "@address function uint32 sync_%d(uint32 selector) const {" % helper_index,
                "    return %s;" % expression,
                "}",
            ]
        )

    filler_function_count = requested_functions - structural_functions
    for filler_index in range(filler_function_count):
        functions.append(
            [
                "@uint32 function filler_%d(uint32 value) {" % filler_index,
                "    observed += value;",
                "}",
            ]
        )

    root_parameters = ["uint32 target", "uint32 selector", *argument_declarations]
    root_body: List[str] = []
    if sync_call_depth:
        root_body.append("uint32 derived = sync_%d(selector);" % (sync_call_depth - 1))
    else:
        root_body.append("uint32 derived = selector;")
    root_body.append("derived += 0u32;")
    root_relays: List[str] = []
    if relay_depth:
        for site_index in range(level_sites[0]):
            offset = site_index + 1
            terms = ["target"] + ["%du32" % (offset + term) for term in range(target_expression_terms - 1)]
            target = " + ".join(terms)
            root_relays.append("relay@(%s) handler_0(%s);" % (target, ", ".join(handler_arguments)))
    if root_relays:
        designated, remainder = root_relays[0:1], root_relays[1:]
        root_body.extend(_wrap_control(designated, branch_depth, loop_depth))
        root_body.extend(remainder)

    # The source statement metric is deterministic and defined as semicolon
    # terminated statements plus branch/loop constructs. Add padding only to
    # the root so function/call/relay structure remains fixed.
    pre_root_lines = [line for function in functions for line in function]
    structural_statement_count = _source_statement_count(pre_root_lines + root_body)
    if requested_statements < structural_statement_count:
        raise GeneratorConfigurationError(
            "statements=%d is below structural minimum %d" % (requested_statements, structural_statement_count)
        )
    for index in range(requested_statements - structural_statement_count):
        root_body.append("derived += %du32;" % ((index % 97) + 1))

    root = [
        "@address function root(%s) export {" % ", ".join(root_parameters),
        *_indent(root_body),
        "}",
    ]
    functions.append(root)

    source_lines = ["contract %s {" % contract, "    @uint32 uint32 observed;", ""]
    for index, function in enumerate(functions):
        source_lines.extend(_indent(function))
        if index + 1 != len(functions):
            source_lines.append("")
    source_lines.append("}")
    source = "\n".join(source_lines) + "\n"
    # Contract state declarations are not executable source statements.
    actual_statements = _source_statement_count(
        [line for function in functions for line in function]
    )
    metadata: Dict[str, Any] = {
        "schema_version": 1,
        "contract": contract,
        "requested": {
            "functions": requested_functions,
            "statements": requested_statements,
            "relay_sites": relay_sites,
            "relay_width": relay_width,
            "relay_depth": relay_depth,
            "branch_depth": branch_depth,
            "loop_depth": loop_depth,
            "sync_call_depth": sync_call_depth,
            "arguments_per_relay": arguments_per_relay,
            "target_expression_terms": target_expression_terms,
        },
        "derived": {
            "functions": len(functions),
            "statements": actual_statements,
            "relay_sites": sum(level_sites),
            "relay_sites_per_level": level_sites,
            "relay_width": max(level_sites) if level_sites else 0,
            "relay_depth": relay_depth,
            "arguments_per_relay": arguments_per_relay,
            "target_expression_terms": target_expression_terms,
            "structural_minimum_functions": structural_functions,
            "structural_minimum_statements": structural_statement_count,
            "statement_metric": "semicolon-terminated source statements plus if/for control statements; for-header semicolons are excluded",
        },
        "source_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
        "deterministic": True,
    }
    if actual_statements != requested_statements:
        raise AssertionError("generator statement accounting drift")
    return source, metadata
