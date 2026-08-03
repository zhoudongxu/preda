#!/usr/bin/env python3
"""End-to-end R-PREDA source mutation, analysis, and strict-trace runner.

The original schema-v5 manifest is an explicit protocol oracle. A mutant is
also compiled with its own manifest so Z3/certificate generation and runtime
strict self-consistency are measured separately. The runner never treats a
mutant's self-consistent strict trace as proof of equivalence to the original.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Sequence, Tuple

from MutationReport import DETECTED, write_json, write_results


HERE = pathlib.Path(__file__).resolve().parent


def _detect_repo_root() -> pathlib.Path:
    """Find the PREDA checkout even when this runner is staged in ``/tmp``."""

    configured = os.environ.get("RPREDA_REPO_ROOT", "")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    for anchor in (HERE, pathlib.Path.cwd().resolve()):
        for candidate in (anchor, *anchor.parents):
            if (candidate / "oxd_preda").is_dir() and (
                candidate / "bin"
            ).is_dir():
                return candidate
    # Do not fall back to ``/``: that would make private absolute paths look
    # repository-relative in publication artifacts.
    return HERE


REPO_ROOT = _detect_repo_root()
DEFAULT_CHSIMU = REPO_ROOT / "bin" / "bin_release" / "chsimu"
DEFAULT_ENGINE = REPO_ROOT / "bin" / "bin_release" / "rpreda_mutation"
DEFAULT_OUTPUT = REPO_ROOT / "results" / "mutation"

COMPILE_FAILURE_RE = re.compile(
    r"\[PRD\]:\s*Compile failed|compile error\s*#|"
    r"Failed to load source code|Invalid deploy|Link failed|link error|"
    r"identifier .* not defined|syntax error",
    re.IGNORECASE,
)
FATAL_RE = re.compile(
    r"Segmentation fault|assertion failed|Engine invoke error|InvokeError|"
    r"Unable to create|Another chain simulator is runnig",
    re.IGNORECASE,
)
GAS_USED_UP_RE = re.compile(r"GasUsedUp|gas\s+used\s+up", re.IGNORECASE)

PIPELINE_DETECTION_ORDER: Sequence[str] = (
    "CompilerRejected",
    "StaticProtocolMismatch",
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
)


class RunnerError(RuntimeError):
    pass


@dataclass
class ProcessResult:
    command: List[str]
    returncode: int | None
    timed_out: bool
    elapsed_seconds: float
    stdout: str
    stderr: str
    launch_error: str = ""


@dataclass
class CompileResult:
    status: str
    reason: str
    process: ProcessResult
    manifest_path: pathlib.Path | None
    manifest: Mapping[str, Any] | None


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False
    )


def digest_json(value: Any) -> str:
    return sha256_bytes(canonical_json(value).encode("utf-8"))


def read_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def portable_path(
    path: pathlib.Path | str | None,
    output_root: pathlib.Path | None = None,
    *,
    retained: bool = True,
) -> str:
    """Return a publication-safe path without embedding a local home directory.

    Source-controlled paths are repository-relative.  Artifacts below an
    external output directory are output-relative.  Large compiler/runtime
    artifacts that the checked-in result intentionally omits are explicitly
    prefixed with ``not-retained/`` rather than rewritten after the run.
    """

    if path is None or str(path) == "":
        return ""
    resolved = pathlib.Path(path).resolve()
    if output_root is not None:
        try:
            relative = resolved.relative_to(output_root.resolve())
            if not retained:
                return (pathlib.PurePosixPath("not-retained") / relative).as_posix()
            try:
                return resolved.relative_to(REPO_ROOT.resolve()).as_posix()
            except ValueError:
                return relative.as_posix()
        except ValueError:
            pass
    try:
        return resolved.relative_to(REPO_ROOT.resolve()).as_posix()
    except ValueError:
        # Preserve a useful basename while never exposing the caller's home.
        return (pathlib.PurePosixPath("external") / resolved.name).as_posix()


def _portable_command_argument(argument: str, output_root: pathlib.Path | None) -> str:
    for prefix in ("-rpreda_trace_report:", "-rpreda_trace_fault:"):
        if argument.startswith(prefix):
            return prefix + portable_path(argument[len(prefix) :], output_root)
    candidate = pathlib.Path(argument)
    if candidate.is_absolute():
        return portable_path(candidate, output_root)
    return argument


def _redact_local_paths(value: str, output_root: pathlib.Path | None) -> str:
    replacements: List[Tuple[str, str]] = []
    if output_root is not None:
        replacements.append((str(output_root.resolve()), "<output>"))
    replacements.append((str(REPO_ROOT.resolve()), "<repo>"))
    home = pathlib.Path.home()
    replacements.append((str(home.resolve()), "<home>"))
    for original, replacement in sorted(
        set(replacements), key=lambda item: len(item[0]), reverse=True
    ):
        value = value.replace(original, replacement)
    return value


def run_process(
    command: Sequence[str],
    cwd: pathlib.Path,
    environment: Mapping[str, str],
    timeout_seconds: float,
) -> ProcessResult:
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            list(command),
            cwd=str(cwd),
            env=dict(environment),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
    except OSError as exc:
        return ProcessResult(
            list(command), None, False, time.monotonic() - started, "", "", str(exc)
        )
    try:
        stdout, stderr = process.communicate(timeout=timeout_seconds)
        return ProcessResult(
            list(command),
            process.returncode,
            False,
            time.monotonic() - started,
            stdout,
            stderr,
        )
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=5)
        except (ProcessLookupError, subprocess.TimeoutExpired):
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            stdout, stderr = process.communicate()
        return ProcessResult(
            list(command),
            process.returncode,
            True,
            time.monotonic() - started,
            stdout,
            stderr,
        )


def persist_process(
    directory: pathlib.Path,
    result: ProcessResult,
    output_root: pathlib.Path | None = None,
) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "stdout.log").write_text(
        _redact_local_paths(result.stdout, output_root), encoding="utf-8"
    )
    (directory / "stderr.log").write_text(
        _redact_local_paths(result.stderr, output_root), encoding="utf-8"
    )
    write_json(
        directory / "process.json",
        {
            "command": [
                _portable_command_argument(argument, output_root)
                for argument in result.command
            ],
            "returncode": result.returncode,
            "timed_out": result.timed_out,
            "elapsed_seconds": result.elapsed_seconds,
            "launch_error": _redact_local_paths(result.launch_error, output_root),
        },
    )


def process_environment(
    chsimu: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
) -> Dict[str, str]:
    environment = dict(os.environ)
    paths = [str(chsimu.parent), *[path for path in library_paths if path]]
    existing = environment.get("LD_LIBRARY_PATH", "")
    if existing:
        paths.append(existing)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths)
    executable_paths = [path for path in path_prefixes if path]
    if environment.get("PATH"):
        executable_paths.append(environment["PATH"])
    environment["PATH"] = os.pathsep.join(executable_paths)
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    return environment


def discover_manifest(home: pathlib.Path) -> pathlib.Path | None:
    directory = home / ".preda" / "chsimu_repo" / "native" / "relay_protocol"
    candidates = sorted(directory.glob("*.relay_protocol.json"))
    return candidates[0] if len(candidates) == 1 else None


def compile_source(
    source: pathlib.Path,
    run_directory: pathlib.Path,
    chsimu: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
    timeout_seconds: float,
    output_root: pathlib.Path | None = None,
) -> CompileResult:
    home = run_directory / "home"
    home.mkdir(parents=True, exist_ok=True)
    environment = process_environment(chsimu, library_paths, path_prefixes)
    environment["HOME"] = str(home)
    command = [
        str(chsimu),
        str(source.resolve()),
        "-rpreda_trace:off",
        "-stdout",
    ]
    process = run_process(command, chsimu.parent, environment, timeout_seconds)
    persist_process(run_directory, process, output_root)
    combined = process.stdout + "\n" + process.stderr
    if process.launch_error:
        return CompileResult(
            "InfrastructureFailure", process.launch_error, process, None, None
        )
    if process.timed_out:
        return CompileResult(
            "InfrastructureFailure", "compiler invocation timed out", process, None, None
        )
    if process.returncode is None or process.returncode < 0:
        return CompileResult(
            "InfrastructureFailure",
            f"compiler terminated by signal ({process.returncode})",
            process,
            None,
            None,
        )
    if COMPILE_FAILURE_RE.search(combined):
        return CompileResult(
            "CompilerRejected",
            "PREDA compiler reported a compile failure",
            process,
            None,
            None,
        )
    if FATAL_RE.search(combined):
        return CompileResult(
            "InfrastructureFailure",
            "fatal simulator diagnostic during compilation",
            process,
            None,
            None,
        )
    manifest_path = discover_manifest(home)
    if manifest_path is None:
        return CompileResult(
            "InfrastructureFailure",
            "compile produced no unique top-level relay manifest",
            process,
            None,
            None,
        )
    try:
        manifest = read_json(manifest_path)
    except (OSError, json.JSONDecodeError) as exc:
        return CompileResult(
            "InfrastructureFailure",
            f"cannot read compiled relay manifest: {exc}",
            process,
            manifest_path,
            None,
        )
    return CompileResult("Compiled", "", process, manifest_path, manifest)


def _expression_projection(expression: Any) -> Any:
    if not isinstance(expression, Mapping):
        return None
    return {
        "kind": expression.get("kind"),
        "text": expression.get("text", expression.get("source_text", "")),
        "type": expression.get("type", ""),
        "operator": expression.get("operator", ""),
        "literal_value": expression.get("literal_value", ""),
        "children": [
            _expression_projection(child)
            for child in expression.get("children", [])
        ],
    }


def _handler_projection(handler: Mapping[str, Any] | None) -> Any:
    if not isinstance(handler, Mapping):
        return None
    kind = handler.get("kind")
    return {
        "kind": kind,
        "name": handler.get("name") if kind == "named" else "<lambda>",
        "scope": handler.get("scope"),
        "parameter_types": list(handler.get("parameter_types", [])),
        "resolved": bool(handler.get("resolved", False)),
        "may_emit_relay": bool(handler.get("may_emit_relay", False)),
    }


def canonical_protocol(manifest: Mapping[str, Any]) -> Mapping[str, Any]:
    handlers = {
        str(handler.get("id")): handler
        for handler in manifest.get("handlers", [])
        if isinstance(handler, Mapping)
    }
    sites: List[Mapping[str, Any]] = []
    raw_sites = [
        site for site in manifest.get("relay_sites", []) if isinstance(site, Mapping)
    ]
    # The collector array is the owning structural sequence. IDs/ordinals and
    # byte offsets are deliberately excluded; preserving array order still
    # exposes RelayOrderSwap without making whitespace shifts look semantic.
    for site in raw_sites:
        arguments = []
        for argument in site.get("arguments", []):
            arguments.append(
                {
                    "type": argument.get("type"),
                    "expression": _expression_projection(argument.get("expression")),
                }
            )
        branches = []
        for branch in site.get("branches", []):
            branches.append(
                {
                    "condition": _expression_projection(branch.get("condition")),
                    "polarity": bool(branch.get("polarity", True)),
                    "arm": branch.get("arm"),
                }
            )
        loops = []
        for loop in site.get("loops", []):
            loops.append(
                {
                    "kind": loop.get("kind"),
                    "initializer": _expression_projection(loop.get("initializer")),
                    "condition": _expression_projection(loop.get("condition")),
                    "update": _expression_projection(loop.get("update")),
                    "statically_bounded": loop.get("statically_bounded"),
                    "induction_variable": loop.get("induction_variable"),
                    "initial_value": loop.get("initial_value"),
                    "comparison": loop.get("comparison"),
                    "bound_value": loop.get("bound_value"),
                    "step": loop.get("step"),
                }
            )
        sites.append(
            {
                "source_function": site.get(
                    "source_function_signature", site.get("source_function_id")
                ),
                "source_scope": site.get("source_scope"),
                "relay_kind": site.get("relay_kind"),
                "target": _expression_projection(site.get("target")),
                "target_scope": site.get("target_scope"),
                "handler": _handler_projection(handlers.get(str(site.get("handler_id")))),
                "arguments": arguments,
                "branches": branches,
                "loops": loops,
            }
        )
    return {
        "contract": manifest.get("contract"),
        "sites": sites,
    }


def protocol_difference(
    baseline: Mapping[str, Any], mutant: Mapping[str, Any]
) -> Mapping[str, Any]:
    baseline_projection = canonical_protocol(baseline)
    mutant_projection = canonical_protocol(mutant)
    baseline_digest = digest_json(baseline_projection)
    mutant_digest = digest_json(mutant_projection)
    return {
        "changed": baseline_digest != mutant_digest,
        "baseline_digest": baseline_digest,
        "mutant_digest": mutant_digest,
        "baseline_site_count": len(baseline_projection["sites"]),
        "mutant_site_count": len(mutant_projection["sites"]),
    }


def _site_start(site: Mapping[str, Any]) -> int:
    return int(site.get("location", {}).get("start_offset", -1))


def _codepoint_to_byte(text: str, offset: int) -> int:
    if offset < 0:
        return offset
    return len(text[:offset].encode("utf-8"))


def _byte_to_codepoint(text: str, offset: int) -> int:
    if offset < 0:
        return offset
    encoded = text.encode("utf-8")
    if offset > len(encoded):
        return -1
    try:
        return len(encoded[:offset].decode("utf-8"))
    except UnicodeDecodeError:
        return -1


def _generated_edits(generated: Mapping[str, Any] | None) -> List[Mapping[str, Any]]:
    if not isinstance(generated, Mapping):
        return []
    edits = [edit for edit in generated.get("edits", []) if isinstance(edit, Mapping)]
    return sorted(
        edits,
        key=lambda edit: (
            int(edit.get("byte_start_offset", -1)),
            int(edit.get("byte_end_offset", -1)),
        ),
    )


def _edit_delta(edit: Mapping[str, Any]) -> int:
    start = int(edit.get("byte_start_offset", -1))
    end = int(edit.get("byte_end_offset", -1))
    original_size = 0 if end < start else end - start + 1
    replacement_size = len(str(edit.get("replacement", "")).encode("utf-8"))
    return replacement_size - original_size


def _output_start_of_edit(
    selected: Mapping[str, Any], edits: Sequence[Mapping[str, Any]]
) -> int:
    selected_start = int(selected.get("byte_start_offset", -1))
    delta = 0
    for edit in edits:
        if edit is selected:
            break
        if int(edit.get("byte_start_offset", -1)) <= selected_start:
            delta += _edit_delta(edit)
    return selected_start + delta


def _map_original_byte_offset(
    offset: int, edits: Sequence[Mapping[str, Any]]
) -> int:
    delta = 0
    for edit in edits:
        start = int(edit.get("byte_start_offset", -1))
        end = int(edit.get("byte_end_offset", -1))
        if end < start:  # insertion
            if start <= offset:
                delta += _edit_delta(edit)
            continue
        if offset < start:
            break
        if offset > end:
            delta += _edit_delta(edit)
            continue
        # An anchor inside a replacement maps to the replacement start.  Relay
        # OrderSwap is handled separately below so semantic sites follow the
        # moved statement rather than the old source position.
        return start + delta
    return offset + delta


def _load_generated_sources(
    generated: Mapping[str, Any] | None,
) -> Tuple[str, str]:
    if not isinstance(generated, Mapping):
        return "", ""
    values: List[str] = []
    for field in ("original_code_path", "mutated_code_path"):
        try:
            path = pathlib.Path(str(generated.get(field, "")))
            values.append(path.read_text(encoding="utf-8") if path.is_file() else "")
        except OSError:
            values.append("")
    return values[0], values[1]


def _mapped_site_start(
    site: Mapping[str, Any],
    generated: Mapping[str, Any] | None,
    original_source: str,
    mutated_source: str,
) -> int | None:
    site_id = str(site.get("id", ""))
    mutation_type = str((generated or {}).get("mutation_type", ""))
    affected = {str(value) for value in (generated or {}).get("relay_site_ids", [])}
    if mutation_type == "RelayDelete" and site_id in affected:
        return None

    edits = _generated_edits(generated)
    original_start_cp = _site_start(site)
    original_end_cp = int(site.get("location", {}).get("end_offset", -1))
    original_start = (
        _codepoint_to_byte(original_source, original_start_cp)
        if original_source
        else original_start_cp
    )
    original_end = (
        _codepoint_to_byte(original_source, original_end_cp + 1) - 1
        if original_source and original_end_cp >= original_start_cp
        else original_end_cp
    )

    mapped_byte: int | None = None
    if mutation_type == "RelayOrderSwap" and site_id in affected:
        source_edit = next(
            (
                edit
                for edit in edits
                if int(edit.get("byte_start_offset", -1)) == original_start
                and int(edit.get("byte_end_offset", -1)) == original_end
            ),
            None,
        )
        if source_edit is not None:
            expected = str(source_edit.get("expected_original", ""))
            destination = next(
                (
                    edit
                    for edit in edits
                    if edit is not source_edit
                    and str(edit.get("replacement", "")) == expected
                    and str(edit.get("expected_original", ""))
                    == str(source_edit.get("replacement", ""))
                ),
                None,
            )
            if destination is not None:
                mapped_byte = _output_start_of_edit(destination, edits)
    if mapped_byte is None:
        mapped_byte = _map_original_byte_offset(original_start, edits)
    if mutated_source:
        return _byte_to_codepoint(mutated_source, mapped_byte)
    return mapped_byte


def _site_fingerprint(
    site: Mapping[str, Any], handlers: Mapping[str, Mapping[str, Any]]
) -> str:
    handler = handlers.get(str(site.get("handler_id", "")))
    return canonical_json(
        {
            "source_function_id": site.get("source_function_id"),
            "source_scope": site.get("source_scope"),
            "relay_kind": site.get("relay_kind"),
            "target_scope": site.get("target_scope"),
            "target": _expression_projection(site.get("target")),
            "handler": _handler_projection(handler),
            "arguments": [
                {
                    "type": argument.get("type"),
                    "expression": _expression_projection(argument.get("expression")),
                }
                for argument in site.get("arguments", [])
            ],
            "branches": [
                {
                    "condition": _expression_projection(branch.get("condition")),
                    "polarity": branch.get("polarity"),
                }
                for branch in site.get("branches", [])
            ],
            "loops": site.get("loops", []),
        }
    )


def _align_surviving_sites(
    baseline: Mapping[str, Any],
    mutant: Mapping[str, Any],
    generated: Mapping[str, Any] | None,
) -> Dict[str, str | None]:
    original_source, mutated_source = _load_generated_sources(generated)
    mutant_sites = [
        site for site in mutant.get("relay_sites", []) if isinstance(site, Mapping)
    ]
    by_anchor: Dict[Tuple[str, int], List[Mapping[str, Any]]] = {}
    for site in mutant_sites:
        key = (str(site.get("source_function_id", "")), _site_start(site))
        by_anchor.setdefault(key, []).append(site)
    baseline_handlers = {
        str(handler.get("id", "")): handler
        for handler in baseline.get("handlers", [])
        if isinstance(handler, Mapping)
    }
    mutant_handlers = {
        str(handler.get("id", "")): handler
        for handler in mutant.get("handlers", [])
        if isinstance(handler, Mapping)
    }
    by_fingerprint: Dict[str, List[Mapping[str, Any]]] = {}
    for site in mutant_sites:
        by_fingerprint.setdefault(_site_fingerprint(site, mutant_handlers), []).append(site)

    result: Dict[str, str | None] = {}
    used: set[str] = set()
    baseline_sites = [
        site for site in baseline.get("relay_sites", []) if isinstance(site, Mapping)
    ]
    baseline_sites.sort(key=lambda site: (str(site.get("source_function_id", "")), _site_start(site)))
    for site in baseline_sites:
        site_id = str(site.get("id", ""))
        mapped_start = _mapped_site_start(
            site, generated, original_source, mutated_source
        )
        if mapped_start is None:
            result[site_id] = None
            continue
        function_id = str(site.get("source_function_id", ""))
        candidates = [
            candidate
            for candidate in by_anchor.get((function_id, mapped_start), [])
            if str(candidate.get("id", "")) not in used
        ]
        if not candidates:
            fingerprint = _site_fingerprint(site, baseline_handlers)
            candidates = [
                candidate
                for candidate in by_fingerprint.get(fingerprint, [])
                if str(candidate.get("id", "")) not in used
            ]
        if len(candidates) == 1:
            mutant_id = str(candidates[0].get("id", ""))
            result[site_id] = mutant_id
            used.add(mutant_id)
        else:
            result[site_id] = None
    return result


def _finite_bound(value: Any) -> Tuple[int, int] | None:
    if not isinstance(value, Mapping):
        return None
    upper = value.get("upper_bound")
    if isinstance(upper, Mapping) and upper.get("kind") == "constant":
        return (int(upper.get("value", 0)), 0)
    if value.get("bound_kind") == "Constant":
        return (int(value.get("constant_term", 0)), 0)
    if value.get("bound_kind") == "AffineActiveShardCount":
        return (
            int(value.get("constant_term", 0)),
            int(value.get("active_shard_count_coefficient", 0)),
        )
    return None


def _certificate_projection(manifest: Mapping[str, Any]) -> Mapping[str, Any]:
    certificate = manifest.get("parallel_certificate", {})
    symmetric_pairs: set[Tuple[str, str, str, str]] = set()
    precedence: set[Tuple[str, str, str]] = set()
    bounds: Dict[Tuple[str, str], Tuple[int, int]] = {}
    for function in certificate.get("functions", []):
        function_id = str(function.get("source_function_id", ""))
        for relation in function.get("pair_relations", []):
            if relation.get("status") != "Proved":
                continue
            left = str(relation.get("site_a", ""))
            right = str(relation.get("site_b", ""))
            kind = str(relation.get("relation", ""))
            if kind in {"MutuallyExclusive", "CoEmissionIndependent"}:
                first, second = sorted((left, right))
                symmetric_pairs.add((function_id, first, second, kind))
            elif kind == "MustPrecedeAB":
                precedence.add((function_id, left, right))
            elif kind == "MustPrecedeBA":
                precedence.add((function_id, right, left))
        for field in (
            "direct_logical_work",
            "transitive_logical_work",
            "physical_route_work",
            "relay_tree_depth",
        ):
            bound = _finite_bound(function.get(field))
            if bound is not None:
                bounds[(function_id, field)] = bound
    return {
        "symmetric_pairs": symmetric_pairs,
        "precedence": precedence,
        "bounds": bounds,
    }


def certificate_regressions(
    baseline: Mapping[str, Any],
    mutant: Mapping[str, Any],
    generated: Mapping[str, Any] | None = None,
) -> List[str]:
    original = _certificate_projection(baseline)
    changed = _certificate_projection(mutant)
    alignment = _align_surviving_sites(baseline, mutant, generated)
    regressions: List[str] = []
    for function_id, left, right, relation in sorted(original["symmetric_pairs"]):
        mapped_left = alignment.get(left)
        mapped_right = alignment.get(right)
        # Removing a participant is already a static protocol mismatch.  It is
        # not evidence that the certificate for surviving sites is unsound.
        if mapped_left is None or mapped_right is None:
            continue
        first, second = sorted((mapped_left, mapped_right))
        expected = (function_id, first, second, relation)
        if expected not in changed["symmetric_pairs"]:
            regressions.append(
                f"lost proved {relation} guarantee for surviving sites "
                f"{left}->{mapped_left}, {right}->{mapped_right}"
            )
    for function_id, before, after in sorted(original["precedence"]):
        mapped_before = alignment.get(before)
        mapped_after = alignment.get(after)
        if mapped_before is None or mapped_after is None:
            continue
        expected = (function_id, mapped_before, mapped_after)
        if expected in changed["precedence"]:
            continue
        reverse = (function_id, mapped_after, mapped_before)
        if reverse in changed["precedence"]:
            regressions.append(
                f"proved precedence reversed for surviving sites "
                f"{before}->{mapped_before}, {after}->{mapped_after}"
            )
        else:
            regressions.append(
                f"lost proved precedence for surviving sites "
                f"{before}->{mapped_before}, {after}->{mapped_after}"
            )
    for key, original_bound in sorted(original["bounds"].items()):
        mutant_bound = changed["bounds"].get(key)
        if mutant_bound is None:
            regressions.append(f"lost finite certificate bound {key}")
        elif mutant_bound[1] > original_bound[1] or (
            mutant_bound[1] == original_bound[1]
            and mutant_bound[0] > original_bound[0]
        ):
            regressions.append(
                f"certificate bound increased for {key}: "
                f"{original_bound} -> {mutant_bound}"
            )
    return regressions


def inspect_z3(manifest: Mapping[str, Any], require_z3: bool) -> Mapping[str, Any]:
    obligations = manifest.get("refinement", {}).get("proof_obligations", [])
    safety_disproved: List[str] = []
    unsupported: List[str] = []
    infrastructure: List[str] = []
    statuses: Dict[str, int] = {}
    solver_goals = 0
    z3_observed = False
    for obligation in obligations:
        if obligation.get("proof_role") != "SolverGoal":
            continue
        solver_goals += 1
        result = obligation.get("solver_result", {})
        status = str(result.get("status", "NotRun"))
        backend = str(result.get("backend", "none"))
        statuses[status] = statuses.get(status, 0) + 1
        z3_observed = z3_observed or backend == "z3"
        kind = str(obligation.get("kind", ""))
        obligation_id = str(obligation.get("id", ""))
        if status == "Disproved" and kind == "RelayCountUpperBound":
            safety_disproved.append(obligation_id)
        elif status in {"EncodingError", "InconsistentAssumptions"}:
            infrastructure.append(f"{obligation_id}: {status}")
        elif status in {"Unknown", "Unsupported"}:
            unsupported.append(f"{obligation_id}: {status}")
        elif status == "NotRun" and require_z3:
            infrastructure.append(f"{obligation_id}: Z3 goal was not run")
    if require_z3 and solver_goals and not z3_observed:
        infrastructure.append("no Z3 backend result was observed")
    return {
        "solver_goal_count": solver_goals,
        "z3_observed": z3_observed,
        "statuses": statuses,
        "safety_disproved": safety_disproved,
        "unsupported": unsupported,
        "infrastructure_failures": sorted(set(infrastructure)),
    }


def inspect_trace(report: Mapping[str, Any]) -> Mapping[str, Any]:
    certificate_mismatches: List[str] = []
    runtime_mismatches: List[str] = []
    certificate_mismatch_details: List[Mapping[str, Any]] = []
    runtime_mismatch_details: List[Mapping[str, Any]] = []
    fault_applications: List[Mapping[str, Any]] = []
    mismatches_by_kind: Dict[str, List[str]] = {}
    certificate_mismatches_by_kind: Dict[str, List[str]] = {}
    infrastructure: List[str] = []
    skipped = 0
    passed = 0
    for result in report.get("validation_results", []):
        status = result.get("status")
        detail = result.get("detail", {})
        property_id = str(detail.get("property_id", ""))
        if status == "NotApplicable" and property_id.startswith("runtime_fault."):
            fault_applications.append(
                {
                    "mutation_id": property_id[len("runtime_fault.") :],
                    "kind": str(detail.get("expected", "")),
                    "status": str(detail.get("actual", "")),
                    "source_function_id": str(detail.get("function", "")),
                    "relay_site_id": str(detail.get("relay_site_id", "")),
                    "root_trace_tx_id": detail.get("root_trace_tx_id"),
                    "parent_trace_tx_id": detail.get("parent_trace_tx_id"),
                    "occurrence_index": detail.get("occurrence_index"),
                    "reason": str(
                        detail.get("diagnostic_reason")
                        or result.get("reason", "")
                    ),
                }
            )
            continue
        identity = str(
            detail.get("certificate_id")
            or detail.get("property_id")
            or detail.get("relay_site_id")
            or detail.get("check_kind")
            or "unknown"
        )
        if status == "Passed":
            passed += 1
        elif status == "SkippedUnsupported":
            skipped += 1
        elif status == "Mismatch":
            check_kind = str(
                result.get("check_kind")
                or detail.get("check_kind")
                or "unknown"
            )
            mismatches_by_kind.setdefault(check_kind, []).append(identity)
            mismatch = {
                "identity": identity,
                "check_kind": check_kind,
                "certificate_id": str(detail.get("certificate_id", "")),
                "relay_site_id": str(detail.get("relay_site_id", "")),
                "expected": str(detail.get("expected", "")),
                "actual": str(detail.get("actual", "")),
            }
            if detail.get("certificate_id"):
                certificate_mismatches.append(identity)
                certificate_mismatch_details.append(mismatch)
                certificate_mismatches_by_kind.setdefault(
                    check_kind, []
                ).append(identity)
            else:
                runtime_mismatches.append(identity)
                runtime_mismatch_details.append(mismatch)
        elif status in {
            "ManifestNotLoaded",
            "ManifestBindingMismatch",
            "TraceInstrumentationError",
        }:
            infrastructure.append(f"{status}: {identity}")
    observed_functions = {
        str(event.get("source_function_id", ""))
        for collection in (
            report.get("logical_relay_emissions", []),
            report.get("relay_executions", []),
        )
        for event in collection
        if isinstance(event, Mapping) and event.get("source_function_id")
    }
    observed_sites = {
        str(event.get("relay_site_id", ""))
        for event in report.get("logical_relay_emissions", [])
        if isinstance(event, Mapping) and event.get("relay_site_id")
    }
    return {
        "passed": passed,
        "skipped_unsupported": skipped,
        "certificate_mismatches": certificate_mismatches,
        "runtime_mismatches": runtime_mismatches,
        "certificate_mismatch_details": certificate_mismatch_details,
        "runtime_mismatch_details": runtime_mismatch_details,
        "fault_applications": fault_applications,
        "mismatches_by_kind": {
            key: sorted(set(values))
            for key, values in sorted(mismatches_by_kind.items())
        },
        "certificate_mismatches_by_kind": {
            key: sorted(set(values))
            for key, values in sorted(
                certificate_mismatches_by_kind.items()
            )
        },
        "infrastructure_failures": infrastructure,
        "observed_function_ids": sorted(observed_functions),
        "observed_relay_site_ids": sorted(observed_sites),
        "trace_counters": dict(report.get("counters", {})),
    }


def inspect_runtime_coverage(
    inspection: Mapping[str, Any],
    requirements: Mapping[str, Any] | None,
) -> Mapping[str, Any]:
    requirements = requirements or {}
    required_functions = {
        str(value) for value in requirements.get("required_function_ids", []) if value
    }
    required_sites = {
        str(value) for value in requirements.get("required_site_ids", []) if value
    }
    unresolved_sites = [
        str(value) for value in requirements.get("unresolved_site_ids", []) if value
    ]
    observed_functions = set(inspection.get("observed_function_ids", []))
    observed_sites = set(inspection.get("observed_relay_site_ids", []))
    missing_functions = sorted(required_functions - observed_functions)
    missing_sites = sorted(required_sites - observed_sites)
    return {
        "label": str(requirements.get("label", "")),
        "required_function_ids": sorted(required_functions),
        "required_relay_site_ids": sorted(required_sites),
        "observed_function_ids": sorted(observed_functions),
        "observed_relay_site_ids": sorted(observed_sites),
        "missing_function_ids": missing_functions,
        "missing_relay_site_ids": missing_sites,
        "unresolved_relay_site_ids": sorted(unresolved_sites),
        "covered": not missing_functions and not missing_sites and not unresolved_sites,
    }


def mutation_coverage_requirements(
    generated: Mapping[str, Any],
    baseline_manifest: Mapping[str, Any],
    mutant_manifest: Mapping[str, Any],
) -> Mapping[str, Any]:
    mutation_type = str(generated.get("mutation_type", ""))
    affected_sites = [str(value) for value in generated.get("relay_site_ids", [])]
    alignment = _align_surviving_sites(
        baseline_manifest, mutant_manifest, generated
    )
    # RelayDelete deliberately removes occurrence identity. Recursion inserts
    # a new site rather than preserving the selected seed site's identity.
    function_only = {
        "RelayDelete",
        "IntroduceRelayRecursion",
    }
    required_sites: List[str] = []
    unresolved: List[str] = []
    if mutation_type not in function_only:
        for site_id in affected_sites:
            mapped = alignment.get(site_id)
            if mapped:
                required_sites.append(mapped)
            else:
                unresolved.append(site_id)
    if mutation_type in {"RelayDuplicate", "IntroduceRelayRecursion"}:
        mapped_mutant_sites = {
            value for value in alignment.values() if value is not None
        }
        source_function_id = str(generated.get("source_function_id", ""))
        required_sites.extend(
            str(site.get("id", ""))
            for site in mutant_manifest.get("relay_sites", [])
            if isinstance(site, Mapping)
            and str(site.get("source_function_id", "")) == source_function_id
            and str(site.get("id", "")) not in mapped_mutant_sites
        )
    function_id = str(generated.get("source_function_id", ""))
    return {
        "label": str(generated.get("mutation_id", mutation_type)),
        "required_function_ids": [function_id] if function_id else [],
        "required_site_ids": sorted(set(required_sites)),
        "unresolved_site_ids": sorted(set(unresolved)),
    }


RUNTIME_FAULT_EXPECTED_CHECKS = {
    "RuntimeTargetScopeCorruption": {"target_scope_kind"},
    "RuntimeRelayDuplicate": {"direct_count", "count_upper_bound"},
}


def inspect_runtime_fault(
    inspection: Mapping[str, Any],
    fault_spec: Mapping[str, Any] | None,
    process_returncode: int | None,
    gas_used_up: bool,
) -> Mapping[str, Any]:
    """Gate a controlled validation-slice fault against independent checks."""

    if fault_spec is None:
        return {"requested": False, "semantic_runtime_detected": False}
    mutation_id = str(fault_spec.get("mutation_id", ""))
    kind = str(fault_spec.get("kind", ""))
    applications = [
        value
        for value in inspection.get("fault_applications", [])
        if str(value.get("mutation_id", "")) == mutation_id
    ]
    applied = any(value.get("status") == "Applied" for value in applications)
    invalid = any(
        value.get("status") in {"Invalid", "NotApplied"}
        for value in applications
    )
    expected = RUNTIME_FAULT_EXPECTED_CHECKS.get(kind, set())
    observed = {
        str(value.get("check_kind", ""))
        for value in inspection.get("runtime_mismatch_details", [])
    }
    matching = sorted(expected & observed)
    detected = bool(
        applied
        and matching
        and process_returncode == 2
        and not gas_used_up
        and not inspection.get("infrastructure_failures")
    )
    return {
        "requested": True,
        "mutation_id": mutation_id,
        "kind": kind,
        "applications": applications,
        "applied": applied,
        "invalid_or_not_applied": invalid or not applied,
        "expected_check_kinds": sorted(expected),
        "matching_mismatch_check_kinds": matching,
        "semantic_runtime_detected": detected,
    }


def baseline_coverage_requirements(
    manifest: Mapping[str, Any],
) -> Mapping[str, Any]:
    sites = [
        site for site in manifest.get("relay_sites", []) if isinstance(site, Mapping)
    ]
    return {
        "label": "original_source",
        "required_function_ids": sorted(
            {str(site.get("source_function_id", "")) for site in sites}
        ),
        "required_site_ids": sorted(str(site.get("id", "")) for site in sites),
        "unresolved_site_ids": [],
    }


def run_runtime_strict(
    source: pathlib.Path,
    template_path: pathlib.Path,
    run_directory: pathlib.Path,
    chsimu: pathlib.Path,
    library_paths: Sequence[str],
    path_prefixes: Sequence[str],
    timeout_seconds: float,
    runtime_arguments: Sequence[str],
    coverage_requirements: Mapping[str, Any] | None = None,
    output_root: pathlib.Path | None = None,
    fault_spec_path: pathlib.Path | None = None,
) -> Mapping[str, Any]:
    template = template_path.read_text(encoding="utf-8")
    if "{{SOURCE}}" not in template:
        return {
            "status": "InfrastructureFailure",
            "reason": "runtime template has no {{SOURCE}} placeholder",
        }
    run_directory.mkdir(parents=True, exist_ok=True)
    script = run_directory / "mutation_runtime.prdts"
    relative_source = pathlib.Path(
        os.path.relpath(str(source.resolve()), str(script.parent.resolve()))
    ).as_posix()
    script.write_text(
        template.replace("{{SOURCE}}", relative_source),
        encoding="utf-8",
    )
    report_path = run_directory / "trace.json"
    home = run_directory / "home"
    home.mkdir(parents=True, exist_ok=True)
    environment = process_environment(chsimu, library_paths, path_prefixes)
    environment["HOME"] = str(home)
    command = [
        str(chsimu),
        str(script),
        *runtime_arguments,
        "-rpreda_trace:strict",
        f"-rpreda_trace_report:{report_path}",
    ]
    fault_spec = None
    if fault_spec_path is not None:
        try:
            fault_spec = read_json(fault_spec_path)
        except (OSError, json.JSONDecodeError) as exc:
            return {
                "status": "InfrastructureFailure",
                "reason": f"runtime fault spec is unreadable: {exc}",
                "termination_reason": "FaultSpecUnreadable",
            }
        command.append(f"-rpreda_trace_fault:{fault_spec_path.resolve()}")
    command.append("-stdout")
    process = run_process(command, chsimu.parent, environment, timeout_seconds)
    persist_process(run_directory, process, output_root)
    if process.launch_error:
        return {
            "status": "InfrastructureFailure",
            "reason": f"runtime launch failed: {process.launch_error}",
            "termination_reason": "LaunchError",
            "process_elapsed_seconds": process.elapsed_seconds,
        }
    if process.timed_out:
        return {
            "status": "InfrastructureFailure",
            "reason": f"runtime timed out after {process.elapsed_seconds:.3f}s",
            "termination_reason": "Timeout",
            "process_elapsed_seconds": process.elapsed_seconds,
        }
    if process.returncode is None or process.returncode < 0:
        return {
            "status": "InfrastructureFailure",
            "reason": f"runtime terminated by signal ({process.returncode})",
            "termination_reason": "Signal",
            "process_elapsed_seconds": process.elapsed_seconds,
        }
    combined = process.stdout + "\n" + process.stderr
    gas_used_up = bool(GAS_USED_UP_RE.search(combined))
    if COMPILE_FAILURE_RE.search(combined):
        return {
            "status": "CompilerRejected",
            "reason": "runtime fixture compilation rejected the source",
        }
    if FATAL_RE.search(combined) and not report_path.is_file():
        return {
            "status": "InfrastructureFailure",
            "reason": "fatal runtime diagnostic without trace report",
            "termination_reason": "FatalDiagnostic",
            "process_returncode": process.returncode,
            "process_elapsed_seconds": process.elapsed_seconds,
        }
    if not report_path.is_file():
        return {
            "status": "InfrastructureFailure",
            "reason": (
                "runtime execution terminated with GasUsedUp; "
                "strict runtime produced no trace report"
                if gas_used_up
                else "strict runtime produced no trace report"
            ),
            "termination_reason": (
                "GasUsedUp" if gas_used_up else "TraceReportMissing"
            ),
            "process_returncode": process.returncode,
            "process_elapsed_seconds": process.elapsed_seconds,
        }
    try:
        report = read_json(report_path)
    except (OSError, json.JSONDecodeError) as exc:
        return {
            "status": "InfrastructureFailure",
            "reason": f"strict trace report is unreadable: {exc}",
        }
    inspection = inspect_trace(report)
    coverage = inspect_runtime_coverage(inspection, coverage_requirements)
    fault_validation = inspect_runtime_fault(
        inspection, fault_spec, process.returncode, gas_used_up
    )
    reasons: List[str] = []
    if gas_used_up:
        reasons.append("runtime execution terminated with GasUsedUp")
    reasons.extend(inspection.get("infrastructure_failures", []))
    if not coverage["covered"]:
        reasons.append(
            "runtime coverage missing: functions="
            + ",".join(coverage["missing_function_ids"])
            + "; relay_sites="
            + ",".join(coverage["missing_relay_site_ids"])
            + "; unresolved_sites="
            + ",".join(coverage["unresolved_relay_site_ids"])
        )
    if not coverage["covered"]:
        status = "InfrastructureFailure"
    elif fault_validation.get("requested") and gas_used_up:
        status = "InfrastructureFailure"
    elif gas_used_up:
        # A source mutant that exhausts gas is a behavioral runtime kill, not
        # a harness timeout.  Any simultaneous trace instrumentation failure is
        # retained separately so analysis_complete remains false.
        status = "RuntimeStrictMismatch"
    elif inspection["infrastructure_failures"]:
        status = "InfrastructureFailure"
    elif fault_validation.get("requested"):
        if fault_validation.get("semantic_runtime_detected"):
            status = "RuntimeStrictMismatch"
        else:
            status = "Unsupported"
            reasons.append(
                "controlled runtime fault was not independently detected by "
                "an expected non-certificate strict check"
            )
    elif inspection["certificate_mismatches"]:
        status = "CertificateViolation"
    elif inspection["runtime_mismatches"]:
        status = "RuntimeStrictMismatch"
    elif process.returncode != 0:
        status = "InfrastructureFailure"
        reasons.append(
            f"runtime exited with code {process.returncode} "
            "without a classified diagnostic"
        )
    else:
        status = "Passed"
    if fault_validation.get("requested") and gas_used_up:
        termination_reason = "FaultExperimentGasUsedUp"
    elif gas_used_up:
        termination_reason = "GasUsedUp"
    elif inspection["infrastructure_failures"]:
        termination_reason = "TraceInstrumentationError"
    elif not coverage["covered"]:
        termination_reason = "CoverageFailure"
    elif inspection["certificate_mismatches"] or inspection["runtime_mismatches"]:
        termination_reason = "StrictValidationFailure"
    elif fault_validation.get("requested") and not fault_validation.get("applied"):
        termination_reason = "FaultNotApplied"
    elif process.returncode != 0:
        termination_reason = "UnclassifiedNonZeroExit"
    else:
        termination_reason = "Completed"
    return {
        "status": status,
        "reason": "; ".join(reasons),
        "termination_reason": termination_reason,
        "trace_report": portable_path(report_path, output_root, retained=False),
        "process_returncode": process.returncode,
        "process_elapsed_seconds": process.elapsed_seconds,
        "coverage": coverage,
        "fault_validation": fault_validation,
        **inspection,
    }


def choose_classification(detected_by: Iterable[str], unsupported: bool) -> str:
    detections = set(detected_by)
    for classification in PIPELINE_DETECTION_ORDER:
        if classification in detections:
            return classification
    if "InfrastructureFailure" in detections:
        return "InfrastructureFailure"
    return "Unsupported" if unsupported else "Survived"


def run_readonly(command: Sequence[str], cwd: pathlib.Path) -> str:
    try:
        result = subprocess.run(
            list(command),
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
    except OSError:
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def baseline_control(
    compile_result: CompileResult,
    manifest: Mapping[str, Any] | None,
    require_z3: bool,
    runtime: Mapping[str, Any] | None,
) -> Mapping[str, Any]:
    detected_by: List[str] = []
    reasons: List[str] = []
    z3 = inspect_z3(manifest, require_z3) if manifest is not None else {}
    if compile_result.status in {"CompilerRejected", "InfrastructureFailure"}:
        detected_by.append(compile_result.status)
        reasons.append(compile_result.reason)
    if z3.get("safety_disproved"):
        detected_by.append("Z3Disproved")
        reasons.append("baseline safety goal was disproved")
    if z3.get("infrastructure_failures"):
        detected_by.append("InfrastructureFailure")
        reasons.extend(z3["infrastructure_failures"])
    if runtime is not None and runtime.get("status") not in {"Passed", "NotRun"}:
        detected_by.append(str(runtime.get("status")))
        reasons.append(str(runtime.get("reason", "")))
    if runtime is not None and runtime.get("infrastructure_failures"):
        detected_by.append("InfrastructureFailure")
        reasons.extend(str(value) for value in runtime["infrastructure_failures"])
    unsupported = bool(z3.get("unsupported"))
    final = choose_classification(detected_by, unsupported)
    return {
        "control_id": "original_source",
        "final_classification": final,
        "detected_by": sorted(set(detected_by)),
        "reason": "; ".join(reason for reason in reasons if reason),
        "z3": z3,
        "runtime": runtime or {"status": "NotRun"},
    }


def make_record(
    generated: Mapping[str, Any],
    compile_result: CompileResult,
    baseline_manifest: Mapping[str, Any],
    require_z3: bool,
    runtime: Mapping[str, Any] | None,
    output_root: pathlib.Path | None = None,
) -> Mapping[str, Any]:
    portable_generated = dict(generated)
    for path_field in ("original_code_path", "mutated_code_path"):
        if portable_generated.get(path_field):
            portable_generated[path_field] = portable_path(
                str(portable_generated[path_field]), output_root
            )
    detected_by: List[str] = []
    reasons: List[str] = []
    protocol = {"changed": False}
    z3: Mapping[str, Any] = {}
    certificate: Mapping[str, Any] = {"regressions": []}
    unsupported = generated.get("generation_status") != "Generated"

    if compile_result.status in {"CompilerRejected", "InfrastructureFailure"}:
        detected_by.append(compile_result.status)
        reasons.append(compile_result.reason)
    elif compile_result.manifest is not None:
        protocol = protocol_difference(baseline_manifest, compile_result.manifest)
        if protocol["changed"]:
            detected_by.append("StaticProtocolMismatch")
            reasons.append("canonical relay protocol differs from baseline oracle")
        z3 = inspect_z3(compile_result.manifest, require_z3)
        if z3["safety_disproved"]:
            detected_by.append("Z3Disproved")
            reasons.append("solver disproved a safety obligation")
        if z3["infrastructure_failures"]:
            detected_by.append("InfrastructureFailure")
            reasons.extend(z3["infrastructure_failures"])
        unsupported = unsupported or bool(z3["unsupported"])
        regressions = certificate_regressions(
            baseline_manifest, compile_result.manifest, generated
        )
        certificate = {"regressions": regressions}
        if regressions:
            detected_by.append("CertificateViolation")
            reasons.extend(regressions)

    if runtime is not None:
        runtime_status = str(runtime.get("status", "NotRun"))
        if runtime.get("certificate_mismatches"):
            detected_by.append("CertificateViolation")
        fault_validation = runtime.get("fault_validation", {})
        if runtime.get("runtime_mismatches") and (
            not fault_validation.get("requested")
            or fault_validation.get("semantic_runtime_detected")
        ):
            detected_by.append("RuntimeStrictMismatch")
        if runtime_status in {
            "InfrastructureFailure",
            "CompilerRejected",
            "CertificateViolation",
            "RuntimeStrictMismatch",
        }:
            detected_by.append(runtime_status)
            if runtime.get("reason"):
                reasons.append(str(runtime["reason"]))
        if runtime.get("infrastructure_failures") and "InfrastructureFailure" not in detected_by:
            detected_by.append("InfrastructureFailure")
            reasons.extend(str(value) for value in runtime["infrastructure_failures"])
        if runtime_status == "Unsupported":
            unsupported = True

    detected_by = list(dict.fromkeys(detected_by))
    final = choose_classification(detected_by, unsupported)
    detection_method = final if final in DETECTED else ""
    stage_failures = [
        status for status in detected_by if status == "InfrastructureFailure"
    ]
    unsupported_stages: List[str] = []
    if generated.get("generation_status") != "Generated":
        unsupported_stages.append("MutationOperator")
    if z3.get("unsupported"):
        unsupported_stages.append("Z3")
    if int((runtime or {}).get("skipped_unsupported", 0) or 0) > 0:
        unsupported_stages.append("RuntimeStrictValidation")
    return {
        **portable_generated,
        "detection_method": detection_method,
        "final_classification": final,
        "detected_by": detected_by,
        "compile_status": compile_result.status,
        "static_protocol_changed": bool(protocol.get("changed", False)),
        "z3_status": "Disproved"
        if z3.get("safety_disproved")
        else "Unsupported"
        if z3.get("unsupported")
        else "Complete",
        "certificate_status": "Violation"
        if certificate.get("regressions")
        or (runtime or {}).get("certificate_mismatches")
        else "NoRegression",
        "runtime_status": (runtime or {}).get("status", "NotRun"),
        "detection_layers": [
            layer
            for layer, classification in (
                ("static", "StaticProtocolMismatch"),
                ("z3", "Z3Disproved"),
                ("certificate", "CertificateViolation"),
                ("runtime", "RuntimeStrictMismatch"),
            )
            if classification in detected_by
        ],
        "analysis_complete": not stage_failures and not unsupported_stages,
        "stage_failures": stage_failures,
        "unsupported_stages": unsupported_stages,
        "reason": "; ".join(dict.fromkeys(reason for reason in reasons if reason)),
        "analysis": {
            "protocol": protocol,
            "z3": z3,
            "certificate": certificate,
            "runtime": runtime or {"status": "NotRun"},
            "compile_manifest_path": portable_path(
                compile_result.manifest_path, output_root, retained=False
            ),
        },
    }


def publish_generation_metadata(
    index: Mapping[str, Any],
    index_path: pathlib.Path,
    output_root: pathlib.Path,
) -> None:
    """Publish a path-sanitized copy while the runner keeps live absolute paths."""

    public = json.loads(json.dumps(index))
    for field in ("source_path", "manifest_path"):
        if public.get(field):
            public[field] = portable_path(str(public[field]), output_root)
    for record in public.get("mutations", []):
        for field in ("original_code_path", "mutated_code_path"):
            if record.get(field):
                record[field] = portable_path(str(record[field]), output_root)
        mutation_id = str(record.get("mutation_id", ""))
        if mutation_id:
            metadata_path = index_path.parent / "mutants" / mutation_id / "mutation.json"
            if metadata_path.parent.is_dir():
                write_json(metadata_path, record)
    write_json(index_path, public)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--engine", type=pathlib.Path, default=DEFAULT_ENGINE)
    parser.add_argument("--chsimu", type=pathlib.Path, default=DEFAULT_CHSIMU)
    parser.add_argument("--seed", type=int, default=88)
    parser.add_argument("--max-per-kind", type=int, default=32)
    parser.add_argument("--kinds", default="")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--runtime-template",
        required=True,
        type=pathlib.Path,
        help="runtime-strict script template; required for the coverage gate",
    )
    parser.add_argument("--runtime-arg", action="append", default=[])
    parser.add_argument("--library-path", action="append", default=[])
    parser.add_argument("--path-prefix", action="append", default=[])
    parser.add_argument("--allow-z3-disabled", action="store_true")
    parser.add_argument("--include-inapplicable", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    source = args.source.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if not source.is_file():
        raise RunnerError(f"source does not exist: {source}")
    if not args.chsimu.is_file():
        raise RunnerError(f"chsimu does not exist: {args.chsimu}")
    if not args.engine.is_file():
        raise RunnerError(f"mutation engine does not exist: {args.engine}")
    if args.runtime_template is not None and not args.runtime_template.is_file():
        raise RunnerError(f"runtime template does not exist: {args.runtime_template}")

    require_z3 = not args.allow_z3_disabled
    baseline_compile = compile_source(
        source,
        output / "baseline" / "compile",
        args.chsimu.resolve(),
        args.library_path,
        args.path_prefix,
        args.timeout,
        output,
    )
    compiled_baseline = baseline_compile.manifest
    oracle_manifest: Mapping[str, Any] | None = None
    if args.manifest is not None:
        oracle_manifest = read_json(args.manifest.resolve())
        if compiled_baseline is not None and protocol_difference(
            oracle_manifest, compiled_baseline
        )["changed"]:
            baseline_compile = CompileResult(
                "InfrastructureFailure",
                "provided baseline manifest does not match freshly compiled source",
                baseline_compile.process,
                baseline_compile.manifest_path,
                baseline_compile.manifest,
            )
    else:
        oracle_manifest = compiled_baseline

    baseline_runtime = None
    runtime_arguments = args.runtime_arg or [
        f"-seed:{args.seed}",
        "-order:2",
        "-addresses:16",
        "-count:1",
    ]
    if args.runtime_template is not None and baseline_compile.status == "Compiled":
        baseline_runtime = run_runtime_strict(
            source,
            args.runtime_template.resolve(),
            output / "baseline" / "runtime",
            args.chsimu.resolve(),
            args.library_path,
            args.path_prefix,
            args.timeout,
            runtime_arguments,
            baseline_coverage_requirements(oracle_manifest or {}),
            output,
        )
    control = baseline_control(
        baseline_compile, oracle_manifest, require_z3, baseline_runtime
    )

    metadata = {
        "started_at": utc_now(),
        "source": portable_path(source),
        "source_sha256": sha256_file(source),
        "seed": args.seed,
        "max_per_kind": args.max_per_kind,
        "require_z3": require_z3,
        "runtime_template": portable_path(args.runtime_template),
        "chsimu": portable_path(args.chsimu),
        "chsimu_sha256": sha256_file(args.chsimu.resolve()),
        "mutation_engine": portable_path(args.engine),
        "mutation_engine_sha256": sha256_file(args.engine.resolve()),
        "git_commit": run_readonly(["git", "rev-parse", "HEAD"], REPO_ROOT),
        "git_status": run_readonly(
            ["git", "status", "--short", "--untracked-files=no"], REPO_ROOT
        ).splitlines(),
        "git_status_includes_untracked": False,
    }

    if oracle_manifest is None or control["final_classification"] in {
        "InfrastructureFailure",
        "CompilerRejected",
        "Z3Disproved",
        "CertificateViolation",
        "RuntimeStrictMismatch",
    }:
        metadata["completed_at"] = utc_now()
        write_results(output, metadata, [], [control])
        print(
            "Baseline control failed; mutation generation was not started. "
            f"See {output / 'mutation.json'}",
            file=sys.stderr,
        )
        return 2

    oracle_path = output / "baseline" / "oracle.relay_protocol.json"
    write_json(oracle_path, oracle_manifest)
    generation_directory = output / "generation"
    command = [
        str(args.engine.resolve()),
        "--source",
        str(source),
        "--manifest",
        str(oracle_path),
        "--output",
        str(generation_directory),
        "--seed",
        str(args.seed),
        "--max-per-kind",
        str(args.max_per_kind),
    ]
    if args.kinds:
        command.extend(["--kinds", args.kinds])
    if args.include_inapplicable:
        command.append("--include-inapplicable")
    generation_process = run_process(
        command,
        REPO_ROOT,
        dict(os.environ),
        args.timeout,
    )
    persist_process(output / "generation_process", generation_process, output)
    index_path = generation_directory / "mutation_index.json"
    if (
        generation_process.launch_error
        or generation_process.timed_out
        or generation_process.returncode != 0
        or not index_path.is_file()
    ):
        failed_control = {
            "control_id": "mutation_generator",
            "final_classification": "InfrastructureFailure",
            "detected_by": ["InfrastructureFailure"],
            "reason": generation_process.launch_error
            or "mutation generator failed or produced no index",
        }
        metadata["completed_at"] = utc_now()
        write_results(output, metadata, [], [control, failed_control])
        return 2

    index = read_json(index_path)
    publish_generation_metadata(index, index_path, output)
    records: List[Mapping[str, Any]] = []
    mutations = index.get("mutations", [])
    for ordinal, generated in enumerate(mutations, start=1):
        mutation_id = str(generated.get("mutation_id", f"unknown-{ordinal}"))
        print(f"[{ordinal}/{len(mutations)}] {mutation_id}", flush=True)
        if generated.get("generation_status") != "Generated":
            dummy_process = ProcessResult([], 0, False, 0.0, "", "")
            compile_result = CompileResult(
                "NotRun", "operator was inapplicable", dummy_process, None, None
            )
            records.append(
                make_record(
                    generated,
                    compile_result,
                    oracle_manifest,
                    require_z3,
                    None,
                    output,
                )
            )
            continue
        mutant_source = pathlib.Path(str(generated.get("mutated_code_path", "")))
        run_directory = output / "runs" / mutation_id
        compile_result = compile_source(
            mutant_source,
            run_directory / "compile",
            args.chsimu.resolve(),
            args.library_path,
            args.path_prefix,
            args.timeout,
            output,
        )
        runtime = None
        if args.runtime_template is not None and compile_result.status == "Compiled":
            coverage_requirements = mutation_coverage_requirements(
                generated,
                oracle_manifest,
                compile_result.manifest or {},
            )
            runtime = run_runtime_strict(
                mutant_source,
                args.runtime_template.resolve(),
                run_directory / "runtime",
                args.chsimu.resolve(),
                args.library_path,
                args.path_prefix,
                args.timeout,
                runtime_arguments,
                coverage_requirements,
                output,
            )
        record = make_record(
            generated,
            compile_result,
            oracle_manifest,
            require_z3,
            runtime,
            output,
        )
        write_json(run_directory / "result.json", record)
        records.append(record)

    metadata["completed_at"] = utc_now()
    metadata["generated_mutant_count"] = len(mutations)
    payload = write_results(output, metadata, records, [control])
    print(
        f"Mutation study complete: {len(records)} mutants, "
        f"detection_rate={payload['metrics']['overall_detection_rate']}. "
        f"Results: {output / 'mutation.json'}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RunnerError as exc:
        print(f"MutationRunner: {exc}", file=sys.stderr)
        raise SystemExit(2)
