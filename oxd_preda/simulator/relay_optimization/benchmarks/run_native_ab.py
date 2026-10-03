#!/usr/bin/env python3
"""Reproducible Native Engine A/B harness for R-PREDA relay optimization.

The harness deliberately uses one optimization-enabled, trace-disabled chsimu
binary for all four ablations.  Correctness collection and timed sampling are
separate so visualization serialization cannot contaminate performance runs.
"""

import argparse
import collections
import csv
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import random
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple


HERE = pathlib.Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
DEFAULT_CONFIG = HERE / "workloads.json"
DEFAULT_BINARY = REPO_ROOT / "bin" / "bin_release" / "chsimu"
VIZ_TEMPLATE = HERE / "viz_template.html"

VARIANTS: Mapping[str, Tuple[str, Optional[str], Optional[str]]] = {
    "baseline": ("baseline", None, None),
    "generic_batch_only": ("optimize", "generic_batch_only", None),
    "verified_reserve_only": ("optimize", "verified_reserve_only", None),
    "verified_reserve_plus_batch": (
        "optimize",
        "verified_reserve_plus_batch",
    ),
    # These variants keep the same optimization-enabled Native Engine and
    # differ only in relay admission/order policy.
    "scheduler_fifo": (
        "optimize",
        "verified_reserve_plus_batch",
        "fifo",
    ),
    "scheduler_blind_priority": (
        "optimize",
        "verified_reserve_plus_batch",
        "certificate_blind_priority",
    ),
    "scheduler_guided_priority": (
        "optimize",
        "verified_reserve_plus_batch",
        "certificate_guided_priority",
    ),
}

STOPWATCH_RE = re.compile(
    r"Stopwatch:\s*(?P<elapsed>\d+)\s*msec.*?"
    r"TPS:\s*(?P<tps>\d+)\s*,\s*uTPS:\s*(?P<utps>\d+)",
    re.DOTALL,
)
TOTAL_TXN_RE = re.compile(r"Total Txn:\s*(?P<pending>\d+)\s*/\s*(?P<executed>\d+)")
ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
FATAL_LOG_RE = re.compile(
    r"Failed to load source code|Link failed|link error|Contract .* is not found|"
    r"Contract function .* is not found|Unable to create|Invalid deploy|"
    r"Argument .* is undefined|Engine invoke error|InvokeError|"
    r"Segmentation fault|assertion failed",
    re.IGNORECASE,
)

PERFORMANCE_FIELDS = (
    "stopwatch_elapsed_ms",
    "source_tps",
    "utps",
    "wall_elapsed_seconds",
    "max_rss_kib",
    "plan_loads",
    "plan_load_failures",
    "plan_binding_failures",
    "plan_cache_hits",
    "plan_cache_misses",
    "plan_invalidations",
    "plan_reloads",
    "optimization_eligible_lookups",
    "optimization_fallback_lookups",
    "optimization_eligible_invocations",
    "optimization_fallback_invocations",
    "scheduler_selection_calls",
    "scheduler_priority_selections",
    "scheduler_reorders",
    "scheduler_certificate_uses",
    "scheduler_fallbacks",
    "scheduler_decision_time_ns",
    "relay_buffer_reserve_calls",
    "relay_buffer_reserved_elements",
    "relay_buffer_reserve_skipped_unknown",
    "relay_buffer_reserve_skipped_limit",
    "relay_buffer_reserve_failures",
    "broadcast_clone_reserve_calls",
    "relay_generation_time_ns",
    "routing_time_ns",
    "dispatch_time_ns",
    "queue_push_time_ns",
    "plan_lookup_time_ns",
    "reserve_time_ns",
    "queue_single_push_calls",
    "queue_legacy_bulk_push_calls",
    "queue_lock_acquisitions",
    "queue_batch_push_calls",
    "queue_batch_elements",
    "maximum_batch_size",
    "queue_batch_fallbacks",
    "queue_notifications",
    "average_batch_size",
    "relay_buffer_capacity_growth_events",
    "relay_buffer_capacity_misses",
    "logical_relay_emissions",
    "physical_relay_routes",
    "relay_executions",
    "broadcast_physical_clones",
)

CORRECTNESS_FIELDS = (
    "process_success",
    "final_contract_state_sha256",
    "source_transaction_count",
    "logical_relay_count",
    "physical_route_count",
    "relay_execution_count",
    "destination_shard_multiset",
    "confirmed_semantic_transaction_multiset_sha256",
    "per_block_dependency_shape_sha256",
    "broadcast_clone_count",
    "invoke_result_multiset",
    "diagnostic_multiset",
)

CORRECTNESS_NORMALIZATION_POLICY: Mapping[str, Any] = {
    "version": 3,
    "semantic_transaction_removed_fields": [
        "Timestamp",
        "PrevBlock",
        "Height",
        "OriginateHeight",
        "GasBurnt",
    ],
    "block_dependency_projection": {
        "dependency_class_fields": [
            "type",
            "contract",
            "function",
            "origin_shard",
            "destination_shard",
            "result",
        ],
        "causal_delivery_classes": [
            "source",
            "same_shard_same_block",
            "same_shard_later_block",
            "same_shard_invalid_backedge",
            "same_shard_unknown_block",
            "cross_shard_or_scope",
        ],
        "producer_batch_key": [
            "OriginateShardIndex",
            "OriginateHeight",
            "OriginateShardOrder",
            "ShardIndex",
        ],
        "absolute_heights": "used only to recover causal groups, then omitted",
        "destination_batch_order": "preserved",
        "independent_batch_order": "multiset",
        "empty_blocks": "ignored",
    },
    "kitty": {
        "ignored_fields": [
            "birth_time",
            "birthTime",
            "lastBreed",
            "newBornIndex",
        ],
        "schedule_assigned_id_rules": [
            "drop myKitties value id when unsigned value is at least 2^31",
            "drop registerNewBorns relay argument id",
        ],
        "mapping_value_multisets": ["myKitties"],
        "unordered_list_multisets": ["new_borns", "newBorns", "allKitties"],
        "unordered_destination_batches": [
            "__relaylambda_*_registerNewBorns",
        ],
    },
}

FEATURE_COUNTERS = (
    "plan_loads",
    "plan_load_failures",
    "plan_binding_failures",
    "optimization_eligible_invocations",
    "optimization_fallback_invocations",
    "relay_buffer_reserve_calls",
    "relay_buffer_reserve_failures",
    "queue_batch_push_calls",
    "queue_batch_elements",
    "queue_batch_fallbacks",
    "logical_relay_emissions",
)


class HarnessError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def digest_json(value: Any) -> str:
    return sha256_bytes(canonical_json(value).encode("utf-8"))


def write_json(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, indent=2, sort_keys=True, ensure_ascii=False)
        stream.write("\n")
    temporary.replace(path)


def read_json(path: pathlib.Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


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


def git_metadata() -> Dict[str, Any]:
    return {
        "commit": run_readonly(["git", "rev-parse", "HEAD"], REPO_ROOT),
        "branch": run_readonly(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], REPO_ROOT
        ),
        "status_porcelain": run_readonly(
            ["git", "status", "--short", "--untracked-files=all"], REPO_ROOT
        ).splitlines(),
    }


def detect_toolchain_bin() -> Optional[pathlib.Path]:
    configured = os.environ.get("RPREDA_TOOLCHAIN_BIN")
    if configured:
        return pathlib.Path(configured).expanduser().resolve()
    cache = REPO_ROOT / "build-gcc12" / "CMakeCache.txt"
    if cache.is_file():
        match = re.search(
            r"^CMAKE_CXX_COMPILER:FILEPATH=(.+)$",
            cache.read_text(encoding="utf-8", errors="replace"),
            re.MULTILINE,
        )
        if match:
            compiler = pathlib.Path(match.group(1)).resolve()
            if compiler.is_file():
                return compiler.parent
    return None


def parse_parameter_overrides(values: Iterable[str]) -> Dict[str, Dict[str, int]]:
    parsed: Dict[str, Dict[str, int]] = {}
    for value in values:
        match = re.fullmatch(r"([^:]+):([A-Za-z_][A-Za-z0-9_]*)=(\d+)", value)
        if not match:
            raise HarnessError(
                "invalid parameter override {!r}; expected WORKLOAD:name=uint".format(
                    value
                )
            )
        workload, name, number = match.groups()
        parsed.setdefault(workload.lower(), {})[name] = int(number)
    return parsed


def load_workloads(
    config_path: pathlib.Path,
    selected_names: Sequence[str],
    performance_overrides: Mapping[str, Mapping[str, int]],
    correctness_overrides: Mapping[str, Mapping[str, int]],
) -> Tuple[Dict[str, Any], List[Dict[str, Any]]]:
    config = read_json(config_path)
    if config.get("schema_version") != 1:
        raise HarnessError("workload config must use schema_version 1")
    raw_workloads = config.get("workloads")
    if not isinstance(raw_workloads, dict) or not raw_workloads:
        raise HarnessError("workload config contains no workloads")

    name_lookup = {name.lower(): name for name in raw_workloads}
    requested = list(selected_names) or list(raw_workloads.keys())
    workloads: List[Dict[str, Any]] = []
    for requested_name in requested:
        canonical_name = name_lookup.get(requested_name.lower())
        if canonical_name is None:
            raise HarnessError("unknown workload: {}".format(requested_name))
        source = raw_workloads[canonical_name]
        performance_fixture = (
            config_path.parent / source["performance_fixture"]
        ).resolve()
        correctness_fixture = (
            config_path.parent / source["correctness_fixture"]
        ).resolve()
        source_contract_value = source.get("source_contract")
        source_contract = None
        if source_contract_value is not None:
            if not isinstance(source_contract_value, str) or not source_contract_value:
                raise HarnessError(
                    "{}.source_contract must be a non-empty string".format(
                        canonical_name
                    )
                )
            source_contract = (config_path.parent / source_contract_value).resolve()
            if not source_contract.is_file():
                raise HarnessError(
                    "missing source contract for {}: {}".format(
                        canonical_name, source_contract
                    )
                )
        for fixture in (performance_fixture, correctness_fixture):
            if not fixture.is_file():
                raise HarnessError("missing fixture: {}".format(fixture))
        perf = dict(source.get("performance_parameters", {}))
        correctness = dict(source.get("correctness_parameters", {}))
        perf.update(performance_overrides.get(canonical_name.lower(), {}))
        correctness.update(correctness_overrides.get(canonical_name.lower(), {}))
        if canonical_name in {"Ballot", "Kitty"}:
            if "count" in performance_overrides.get(canonical_name.lower(), {}) and (
                "addresses" not in performance_overrides.get(canonical_name.lower(), {})
            ):
                perf["addresses"] = perf["count"]
            if "count" in correctness_overrides.get(canonical_name.lower(), {}) and (
                "addresses" not in correctness_overrides.get(canonical_name.lower(), {})
            ):
                correctness["addresses"] = correctness["count"]
        if canonical_name == "Kitty":
            if perf.get("count", 0) < 2 or perf.get("addresses") != perf.get("count"):
                raise HarnessError("Kitty performance requires addresses == count >= 2")
            if correctness.get("count", 0) < 2 or correctness.get(
                "addresses"
            ) != correctness.get("count"):
                raise HarnessError("Kitty correctness requires addresses == count >= 2")
        if canonical_name == "Ballot":
            if perf.get("addresses") != perf.get("count"):
                raise HarnessError(
                    "Ballot performance requires addresses == count because "
                    "Ballot.vote @all is controlled by address count"
                )
            if correctness.get("addresses") != correctness.get("count"):
                raise HarnessError(
                    "Ballot correctness requires addresses == count because "
                    "Ballot.vote @all is controlled by address count"
                )
        for phase, parameters in (
            ("performance", perf),
            ("correctness", correctness),
        ):
            for required in ("count", "addresses"):
                value = parameters.get(required)
                if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                    raise HarnessError(
                        "{}.{} parameter {} must be a positive integer".format(
                            canonical_name, phase, required
                        )
                    )
            for key, value in parameters.items():
                if (
                    not isinstance(value, int)
                    or isinstance(value, bool)
                    or value < 0
                ):
                    raise HarnessError(
                        "{}.{} parameter {} is not a non-negative integer".format(
                            canonical_name, phase, key
                        )
                    )
        runtime_features = source.get("expected_runtime_features")
        if not isinstance(runtime_features, dict):
            raise HarnessError(
                "{} must define expected_runtime_features".format(canonical_name)
            )
        for feature in ("relay_activity", "generic_batch", "verified_reserve"):
            if not isinstance(runtime_features.get(feature), bool):
                raise HarnessError(
                    "{} expected_runtime_features.{} must be boolean".format(
                        canonical_name, feature
                    )
                )
        source_offset = source.get("measured_source_transaction_offset", 0)
        if (
            not isinstance(source_offset, int)
            or isinstance(source_offset, bool)
            or source_offset < 0
        ):
            raise HarnessError(
                "{} measured_source_transaction_offset must be a non-negative "
                "integer".format(canonical_name)
            )
        workloads.append(
            {
                "name": canonical_name,
                "dataset": source.get("dataset", canonical_name),
                "case": source.get("case", "original"),
                "source_contract": source_contract,
                "performance_fixture": performance_fixture,
                "performance_fixture_sha256": sha256_file(performance_fixture),
                "correctness_fixture": correctness_fixture,
                "correctness_fixture_sha256": sha256_file(correctness_fixture),
                "performance_parameters": perf,
                "correctness_parameters": correctness,
                "measured_source_transaction_offset": source_offset,
                "expected_runtime_features": dict(runtime_features),
            }
        )
    return config, workloads


def select_variants(requested: Sequence[str]) -> List[str]:
    names = list(requested) or [
        "baseline",
        "generic_batch_only",
        "verified_reserve_only",
        "verified_reserve_plus_batch",
    ]
    result: List[str] = []
    for name in names:
        normalized = name.lower()
        if normalized not in VARIANTS:
            raise HarnessError("unknown variant: {}".format(name))
        if normalized not in result:
            result.append(normalized)
    if "baseline" not in result and "scheduler_fifo" not in result:
        raise HarnessError(
            "correctness comparison requires baseline or scheduler_fifo"
        )
    return result


def variant_options(variant: str) -> List[str]:
    mode, ablation, scheduler = VARIANTS[variant]
    options = ["-rpreda_opt:{}".format(mode)]
    if ablation is not None:
        options.append("-rpreda_opt_ablation:{}".format(ablation))
    if scheduler is not None:
        options.append("-rpreda_scheduler:{}".format(scheduler))
    return options


def command_for_run(
    binary: pathlib.Path,
    fixture: pathlib.Path,
    variant: str,
    parameters: Mapping[str, int],
    seed: int,
    order: int,
    report_path: pathlib.Path,
    viz_path: Optional[pathlib.Path],
) -> List[str]:
    command = [str(binary), str(fixture)]
    command.extend(variant_options(variant))
    command.extend(
        [
            "-rpreda_opt_report:{}".format(report_path),
            "-seed:{}".format(seed),
            "-order:{}".format(order),
            "-stdout",
        ]
    )
    for name in sorted(parameters):
        command.append("-{}:{}".format(name, parameters[name]))
    if viz_path is not None:
        command.extend(
            [
                "-viz:{}".format(viz_path),
                "-viz_templ:{}".format(VIZ_TEMPLATE),
            ]
        )
    return command


def parse_resource_file(path: pathlib.Path) -> Dict[str, Optional[float]]:
    values: Dict[str, Optional[float]] = {
        "gnu_time_elapsed_seconds": None,
        "max_rss_kib": None,
        "user_seconds": None,
        "system_seconds": None,
    }
    if not path.is_file():
        return values
    mapping = {
        "elapsed_seconds": "gnu_time_elapsed_seconds",
        "max_rss_kib": "max_rss_kib",
        "user_seconds": "user_seconds",
        "system_seconds": "system_seconds",
    }
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        key, separator, value = line.partition("=")
        if separator and key in mapping:
            try:
                values[mapping[key]] = float(value)
            except ValueError:
                pass
    return values


def proc_tree_rss_kib(root_pid: int) -> Optional[int]:
    """Return a best-effort Linux peak-RSS sum for a live process tree.

    VmHWM is a per-process high-water mark retained by the kernel, so it is less
    likely than instantaneous VmRSS to miss a short peak between polling
    intervals. VmRSS is used only on kernels that do not expose VmHWM.
    """
    pending = [root_pid]
    visited = set()
    total = 0
    observed = False
    while pending:
        pid = pending.pop()
        if pid in visited:
            continue
        visited.add(pid)
        proc = pathlib.Path("/proc") / str(pid)
        try:
            status = (proc / "status").read_text(
                encoding="utf-8", errors="replace"
            )
            children = (proc / "task" / str(pid) / "children").read_text(
                encoding="ascii", errors="replace"
            )
        except OSError:
            continue
        match = re.search(r"^VmHWM:\s*(\d+)\s*kB", status, re.MULTILINE)
        if match is None:
            match = re.search(r"^VmRSS:\s*(\d+)\s*kB", status, re.MULTILINE)
        if match:
            total += int(match.group(1))
            observed = True
        pending.extend(int(child) for child in children.split() if child.isdigit())
    return total if observed else None


def parse_simulator_stdout(text: str) -> Dict[str, Optional[int]]:
    clean = ANSI_RE.sub("", text)
    stopwatch_matches = list(STOPWATCH_RE.finditer(clean))
    total_matches = list(TOTAL_TXN_RE.finditer(clean))
    result: Dict[str, Optional[int]] = {
        "stopwatch_elapsed_ms": None,
        "source_tps": None,
        "utps": None,
        "final_pending_transaction_count": None,
        "final_executed_transaction_count": None,
    }
    if stopwatch_matches:
        match = stopwatch_matches[-1]
        result.update(
            {
                "stopwatch_elapsed_ms": int(match.group("elapsed")),
                "source_tps": int(match.group("tps")),
                "utps": int(match.group("utps")),
            }
        )
    if total_matches:
        match = total_matches[-1]
        result.update(
            {
                "final_pending_transaction_count": int(match.group("pending")),
                "final_executed_transaction_count": int(match.group("executed")),
            }
        )
    return result


def fatal_diagnostic_lines(stdout_text: str, stderr_text: str) -> List[str]:
    clean = ANSI_RE.sub("", stdout_text) + "\n" + ANSI_RE.sub("", stderr_text)
    return [line.strip() for line in clean.splitlines() if FATAL_LOG_RE.search(line)]


def terminate_process_group(process: subprocess.Popen) -> None:
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def validate_variant_activity(
    workload: Mapping[str, Any],
    variant: str,
    optimization_report: Optional[Mapping[str, Any]],
    require_measurement_window: bool = False,
) -> List[str]:
    """Reject nominal ablations that never exercised their advertised path.

    Process-lifetime counters establish artifact loading and reject failures
    anywhere in the run. Performance-path activation is intentionally checked
    against the completed stopwatch window, so setup work cannot make an idle
    timed ablation look active.
    """
    if not isinstance(optimization_report, Mapping):
        return ["optimization report is missing or is not an object"]
    lifetime_counters = optimization_report.get("counters")
    if not isinstance(lifetime_counters, Mapping):
        return ["optimization report counters are missing or are not an object"]

    issues: List[str] = []
    activity_counters = lifetime_counters
    activity_scope = "lifetime"
    if require_measurement_window:
        window = optimization_report.get("measurement_window")
        if not isinstance(window, Mapping) or not isinstance(
            window.get("counters"), Mapping
        ):
            return [
                "completed schema-v2 measurement_window counters are required "
                "for performance activation"
            ]
        activity_counters = window["counters"]
        activity_scope = "measurement_window"

    def validated_values(
        counters: Mapping[str, Any], scope: str
    ) -> Dict[str, Optional[int]]:
        values: Dict[str, Optional[int]] = {}
        for name in FEATURE_COUNTERS:
            value = counters.get(name)
            if (
                not isinstance(value, int)
                or isinstance(value, bool)
                or value < 0
            ):
                issues.append(
                    "{} counter {} is missing or invalid".format(scope, name)
                )
                values[name] = None
            else:
                values[name] = value
        return values

    lifetime_values = validated_values(lifetime_counters, "lifetime")
    activity_values = (
        validated_values(activity_counters, activity_scope)
        if activity_counters is not lifetime_counters
        else lifetime_values
    )

    def is_zero(
        name: str,
        values: Mapping[str, Optional[int]] = activity_values,
        scope: str = activity_scope,
    ) -> None:
        value = values.get(name)
        if value is not None and value != 0:
            issues.append(
                "{} {} must be zero, got {}".format(scope, name, value)
            )

    def is_positive(
        name: str,
        values: Mapping[str, Optional[int]] = activity_values,
        scope: str = activity_scope,
    ) -> None:
        value = values.get(name)
        if value is not None and value <= 0:
            issues.append(
                "{} {} must be positive, got {}".format(scope, name, value)
            )

    features = workload["expected_runtime_features"]
    if features["relay_activity"]:
        is_positive("logical_relay_emissions")

    uses_plan = variant in {
        "verified_reserve_only",
        "verified_reserve_plus_batch",
        "scheduler_fifo",
        "scheduler_blind_priority",
        "scheduler_guided_priority",
    }
    uses_batch = variant in {
        "generic_batch_only",
        "verified_reserve_plus_batch",
        "scheduler_fifo",
        "scheduler_blind_priority",
        "scheduler_guided_priority",
    }

    if uses_plan:
        is_positive("plan_loads", lifetime_values, "lifetime")
        is_positive("optimization_eligible_invocations")
        is_zero("plan_load_failures", lifetime_values, "lifetime")
        is_zero("plan_binding_failures", lifetime_values, "lifetime")
        is_zero("relay_buffer_reserve_failures", lifetime_values, "lifetime")
        if features["verified_reserve"]:
            is_positive("relay_buffer_reserve_calls")
        else:
            is_zero("relay_buffer_reserve_calls")
    else:
        for name in (
            "plan_loads",
            "plan_load_failures",
            "plan_binding_failures",
        ):
            is_zero(name, lifetime_values, "lifetime")
        for name in (
            "optimization_eligible_invocations",
            "optimization_fallback_invocations",
            "relay_buffer_reserve_calls",
            "relay_buffer_reserve_failures",
        ):
            is_zero(name)

    if uses_batch:
        is_zero("queue_batch_fallbacks", lifetime_values, "lifetime")
        if features["generic_batch"]:
            is_positive("queue_batch_push_calls")
            is_positive("queue_batch_elements")
    else:
        is_zero("queue_batch_push_calls", lifetime_values, "lifetime")
        is_zero("queue_batch_elements", lifetime_values, "lifetime")
        is_zero("queue_batch_fallbacks", lifetime_values, "lifetime")

    return issues


def measurement_window_validation(
    optimization_report: Optional[Mapping[str, Any]],
    require_completed_schema_v2: bool = False,
) -> Tuple[bool, str, Optional[str]]:
    """Validate stopwatch metrics.

    Correctness-only compatibility reads may accept a legacy lifetime report.
    Every warmup and measured performance run must provide a completed schema-v2
    stopwatch window; lifetime counters are never a performance substitute.
    """
    if not isinstance(optimization_report, Mapping):
        return False, "missing", "optimization report is missing"
    schema_version = optimization_report.get("report_schema_version", 1)
    if schema_version == 1:
        if require_completed_schema_v2:
            return (
                False,
                "lifetime_schema_v1",
                "performance requires report schema 2 with a completed "
                "measurement_window",
            )
        return True, "lifetime_schema_v1", None
    if schema_version != 2:
        return (
            False,
            "unsupported",
            "unsupported optimization report schema {}".format(schema_version),
        )
    window = optimization_report.get("measurement_window")
    if not isinstance(window, Mapping):
        if not require_completed_schema_v2:
            return True, "not_started", None
        return False, "missing", "schema-v2 measurement_window is missing"
    status = str(window.get("status", "<missing>"))
    if status != "completed" or window.get("metrics_available") is not True:
        if not require_completed_schema_v2:
            return True, status, None
        return (
            False,
            status,
            "measurement window is not completed with metrics available",
        )
    for section in ("counters", "timings_ns", "derived"):
        if not isinstance(window.get(section), Mapping):
            return (
                False,
                status,
                "measurement_window.{} is missing or invalid".format(section),
            )
    return True, status, None


def selected_measurement_scope(
    report: Mapping[str, Any],
) -> Tuple[Mapping[str, Any], Mapping[str, Any], Mapping[str, Any], str]:
    window = report.get("measurement_window")
    if (
        report.get("report_schema_version") == 2
        and isinstance(window, Mapping)
        and window.get("status") == "completed"
        and window.get("metrics_available") is True
        and isinstance(window.get("counters"), Mapping)
        and isinstance(window.get("timings_ns"), Mapping)
        and isinstance(window.get("derived"), Mapping)
    ):
        return (
            window["counters"],
            window["timings_ns"],
            window["derived"],
            "measurement_window",
        )
    # This selector feeds performance CSV/statistics only. Invalid or legacy
    # reports remain visible as failed raw runs but must not contribute lifetime
    # values that look like stopwatch-aligned samples.
    return ({}, {}, {}, "unavailable")


def execute_run(
    args: argparse.Namespace,
    workload: Mapping[str, Any],
    variant: str,
    phase: str,
    index: int,
    parameters: Mapping[str, int],
    run_directory: pathlib.Path,
    collect_viz: bool,
) -> Dict[str, Any]:
    run_directory.mkdir(parents=True, exist_ok=False)
    home = run_directory / "home"
    home.mkdir()
    report_path = run_directory / "optimization_metrics.json"
    viz_path = run_directory / "correctness.html" if collect_viz else None
    stdout_path = run_directory / "stdout.log"
    stderr_path = run_directory / "stderr.log"
    resource_path = run_directory / "resource.txt"

    fixture_key = "correctness_fixture" if collect_viz else "performance_fixture"
    fixture_hash_key = fixture_key + "_sha256"
    fixture_template = workload[fixture_key]
    fixture = fixture_template
    if workload.get("source_contract") is not None:
        template_text = fixture_template.read_text(encoding="utf-8")
        if "{{SOURCE}}" in template_text:
            fixture = run_directory / "runtime_fixture.prdts"
            source_path = pathlib.Path(
                os.path.relpath(
                    str(workload["source_contract"]), str(fixture.parent)
                )
            ).as_posix()
            fixture.write_text(
                template_text.replace("{{SOURCE}}", source_path),
                encoding="utf-8",
            )
    command = command_for_run(
        args.binary,
        fixture,
        variant,
        parameters,
        args.seed,
        args.order,
        report_path,
        viz_path,
    )
    wrapped_command = list(command)
    time_binary = args.time_binary
    if time_binary is not None:
        wrapped_command = [
            str(time_binary),
            "-f",
            "elapsed_seconds=%e\\nmax_rss_kib=%M\\nuser_seconds=%U\\nsystem_seconds=%S",
            "-o",
            str(resource_path),
            "--",
        ] + command
    if args.cpu_list:
        wrapped_command = ["taskset", "-c", args.cpu_list] + wrapped_command

    environment = os.environ.copy()
    environment.update(
        {
            "HOME": str(home),
            "LC_ALL": "C",
            "LANG": "C",
            "TZ": "UTC",
        }
    )
    runtime_library_path = str(args.binary.parent)
    library_paths = [runtime_library_path]
    z3_runtime_path = REPO_ROOT / "build" / "deps" / "z3-4.12.1.0" / "lib"
    if z3_runtime_path.is_dir():
        library_paths.append(str(z3_runtime_path))
    if args.toolchain_bin is not None:
        environment["PATH"] = str(args.toolchain_bin) + os.pathsep + environment.get(
            "PATH", ""
        )
        toolchain_library = args.toolchain_bin.parent / "lib"
        if toolchain_library.is_dir():
            library_paths.append(str(toolchain_library))
    existing_library_path = environment.get("LD_LIBRARY_PATH", "")
    if existing_library_path:
        library_paths.append(existing_library_path)
    environment["LD_LIBRARY_PATH"] = os.pathsep.join(library_paths)

    started_at = utc_now()
    wall_start = time.perf_counter()
    timed_out = False
    sampled_peak_rss_kib: Optional[int] = None
    with stdout_path.open("wb") as stdout_stream, stderr_path.open("wb") as stderr_stream:
        try:
            process = subprocess.Popen(
                wrapped_command,
                cwd=str(args.binary.parent),
                env=environment,
                stdout=stdout_stream,
                stderr=stderr_stream,
                start_new_session=True,
            )
        except OSError as error:
            raise HarnessError("cannot execute simulator: {}".format(error)) from error
        deadline = wall_start + args.timeout_seconds
        while process.poll() is None:
            rss = proc_tree_rss_kib(process.pid)
            if rss is not None:
                sampled_peak_rss_kib = max(sampled_peak_rss_kib or 0, rss)
            if time.perf_counter() >= deadline:
                timed_out = True
                terminate_process_group(process)
                break
            time.sleep(0.02)
        return_code = process.returncode
    wall_elapsed = time.perf_counter() - wall_start

    stdout_text = stdout_path.read_text(encoding="utf-8", errors="replace")
    stderr_text = stderr_path.read_text(encoding="utf-8", errors="replace")
    parsed_stdout = parse_simulator_stdout(stdout_text)
    fatal_diagnostics = fatal_diagnostic_lines(stdout_text, stderr_text)
    resources = parse_resource_file(resource_path)
    resources["sampled_peak_rss_kib"] = sampled_peak_rss_kib
    resources["peak_rss_method"] = (
        "gnu_time"
        if resources["max_rss_kib"] is not None
        else "proc_status_vmhwm_sampled"
    )
    if resources["max_rss_kib"] is None:
        resources["max_rss_kib"] = sampled_peak_rss_kib
    optimization_report = read_json(report_path) if report_path.is_file() else None
    report_config = optimization_report.get("config", {}) if optimization_report else {}
    expected_mode, expected_ablation, expected_scheduler = VARIANTS[variant]
    configured_measured_source_transactions = int(parameters["count"]) + workload[
        "measured_source_transaction_offset"
    ]
    stopwatch_elapsed = parsed_stdout["stopwatch_elapsed_ms"]
    expected_source_tps = (
        configured_measured_source_transactions * 1000 // stopwatch_elapsed
        if stopwatch_elapsed
        else None
    )
    source_tps_matches_configured = bool(
        expected_source_tps is not None
        and parsed_stdout["source_tps"] == expected_source_tps
    )
    report_config_matches = bool(
        optimization_report
        and report_config.get("mode") == expected_mode
        and report_config.get("ablation")
        == (expected_ablation if expected_ablation is not None else "baseline")
        and report_config.get("scheduler", "fifo")
        == (expected_scheduler if expected_scheduler is not None else "fifo")
    )
    require_completed_measurement_window = phase != "correctness"
    (
        measurement_window_valid,
        measurement_window_status,
        measurement_window_issue,
    ) = measurement_window_validation(
        optimization_report,
        require_completed_schema_v2=require_completed_measurement_window,
    )
    feature_activation_issues = validate_variant_activity(
        workload,
        variant,
        optimization_report,
        require_measurement_window=require_completed_measurement_window,
    )
    feature_activation_passed = not feature_activation_issues
    run_succeeded = bool(
        not timed_out
        and return_code == 0
        and optimization_report is not None
        and report_config_matches
        and feature_activation_passed
        and measurement_window_valid
        and not fatal_diagnostics
        and (
            phase == "correctness"
            or parsed_stdout["stopwatch_elapsed_ms"] is not None
        )
        and "Run script successfully" in stdout_text
        and (phase == "correctness" or source_tps_matches_configured)
        and (not collect_viz or (viz_path is not None and viz_path.is_file()))
    )
    record: Dict[str, Any] = {
        "schema_version": 1,
        "workload": workload["name"],
        "dataset": workload.get("dataset", workload["name"]),
        "case": workload.get("case", "original"),
        "source_contract": (
            str(workload["source_contract"])
            if workload.get("source_contract") is not None
            else None
        ),
        "variant": variant,
        "phase": phase,
        "index": index,
        "seed": args.seed,
        "order": args.order,
        "parameters": dict(parameters),
        "configured_measured_source_transactions": configured_measured_source_transactions,
        "expected_source_tps": expected_source_tps,
        "source_tps_matches_configured": source_tps_matches_configured,
        "fixture": str(fixture),
        "fixture_template": str(fixture_template),
        "fixture_sha256": workload[fixture_hash_key],
        "command": command,
        "wrapped_command": wrapped_command,
        "started_at": started_at,
        "completed_at": utc_now(),
        "timed_out": timed_out,
        "return_code": return_code,
        "run_succeeded": run_succeeded,
        "report_config_matches": report_config_matches,
        "feature_activation_passed": feature_activation_passed,
        "feature_activation_issues": feature_activation_issues,
        "measurement_window_valid": measurement_window_valid,
        "measurement_window_status": measurement_window_status,
        "measurement_window_issue": measurement_window_issue,
        "fatal_diagnostics": fatal_diagnostics,
        "wall_elapsed_seconds": wall_elapsed,
        "paths": {
            "run_directory": str(run_directory),
            "stdout": str(stdout_path),
            "stderr": str(stderr_path),
            "optimization_report": str(report_path),
            "resource": str(resource_path),
            "viz": str(viz_path) if viz_path is not None else None,
        },
        "simulator": parsed_stdout,
        "resources": resources,
        "optimization_report": optimization_report,
    }
    write_json(run_directory / "run.json", record)
    return record


def extract_viz_json(path: pathlib.Path) -> Any:
    text = path.read_text(encoding="utf-8", errors="strict")
    marker = "var PREDA_VIZ_LOG = `"
    start = text.find(marker)
    if start < 0:
        raise HarnessError("PREDA_VIZ_LOG marker is missing from {}".format(path))
    start += len(marker)
    end = text.find("`;", start)
    if end < 0:
        raise HarnessError("PREDA_VIZ_LOG terminator is missing from {}".format(path))
    return json.loads(text[start:end].strip())


def normalize_json(value: Any, drop_transaction_placement: bool = False) -> Any:
    volatile_keys = {"Timestamp", "PrevBlock"}
    if drop_transaction_placement:
        volatile_keys.update({"Height", "OriginateHeight", "GasBurnt"})
    if isinstance(value, dict):
        return {
            key: normalize_json(child, drop_transaction_placement)
            for key, child in sorted(value.items())
            if key not in volatile_keys
        }
    if isinstance(value, list):
        return [normalize_json(child, drop_transaction_placement) for child in value]
    return value


def sorted_multiset(values: Iterable[Any]) -> List[Any]:
    return sorted(values, key=canonical_json)


def canonicalize_kitty_semantics(
    value: Any,
    parent_key: Optional[str] = None,
    in_mykitties_value: bool = False,
) -> Any:
    """Remove Kitty scheduling artifacts while retaining contract semantics."""
    ignored_keys = {"birth_time", "birthTime", "lastBreed", "newBornIndex"}
    unordered_list_keys = {"new_borns", "newBorns", "allKitties"}
    if isinstance(value, dict):
        result: Dict[str, Any] = {}
        for key, child in sorted(value.items()):
            if key in ignored_keys:
                continue
            if (
                key == "id"
                and in_mykitties_value
                and isinstance(child, int)
                and not isinstance(child, bool)
                and child >= (1 << 31)
            ):
                continue
            if key == "myKitties" and isinstance(child, dict):
                result[key] = sorted_multiset(
                    canonicalize_kitty_semantics(
                        entry, "myKitties", in_mykitties_value=True
                    )
                    for entry in child.values()
                )
                continue
            normalized = canonicalize_kitty_semantics(
                child, key, in_mykitties_value=in_mykitties_value
            )
            if key in unordered_list_keys and isinstance(normalized, list):
                normalized = sorted_multiset(normalized)
            result[key] = normalized
        return result
    if isinstance(value, list):
        normalized = [
            canonicalize_kitty_semantics(
                child,
                parent_key,
                in_mykitties_value=in_mykitties_value,
            )
            for child in value
        ]
        if parent_key in unordered_list_keys:
            return sorted_multiset(normalized)
        return normalized
    return value


def block_records(viz_json: Any) -> List[Dict[str, Any]]:
    records: List[Dict[str, Any]] = []
    if not isinstance(viz_json, list):
        return records
    for section in viz_json:
        if not isinstance(section, dict) or section.get("type") != "Block":
            continue
        content = section.get("content", [])
        if not isinstance(content, list):
            continue
        records.extend(record for record in content if isinstance(record, dict))
    return records


def state_projection(viz_json: Any, workload: Optional[str] = None) -> List[Dict[str, Any]]:
    result: List[Dict[str, Any]] = []
    if not isinstance(viz_json, list):
        return result
    for section in viz_json:
        if not isinstance(section, dict) or section.get("type") not in {
            "Addr",
            "Shard",
            "Uint_scope",
        }:
            continue
        content = normalize_json(section.get("content", []))
        if workload == "Kitty":
            content = canonicalize_kitty_semantics(content)
        if isinstance(content, list):
            # Query output order across shards/addresses is not a semantic
            # ordering guarantee.  Nested contract arrays remain untouched.
            content = sorted_multiset(content)
        result.append(
            {
                "type": section.get("type"),
                "command": section.get("command"),
                "content": content,
            }
        )
    return sorted_multiset(result)


def semantic_transaction(
    transaction: Mapping[str, Any], workload: Optional[str] = None
) -> Dict[str, Any]:
    result = normalize_json(dict(transaction), drop_transaction_placement=True)
    if workload == "Kitty":
        result = canonicalize_kitty_semantics(result)
        function_name = str(result.get("Function", ""))
        arguments = result.get("Arguments")
        if (
            "registerNewBorns" in function_name
            and isinstance(arguments, dict)
        ):
            # This relay pairs a newly allocated ID with an owner after an
            # unordered aggregation. The pairing order is scheduling-derived;
            # mint relay IDs and stable state IDs remain strict.
            arguments.pop("id", None)
    return result


def transaction_dependency_class(transaction: Mapping[str, Any]) -> Dict[str, Any]:
    """Return the stable class used for per-block dependency-shape checks."""
    destination_shard = transaction.get("ShardIndex")
    return {
        "type": transaction.get("InvokeContextType"),
        "contract": transaction.get("Contract"),
        "function": transaction.get("Function"),
        "origin_shard": transaction.get(
            "OriginateShardIndex", destination_shard
        ),
        "destination_shard": destination_shard,
        "result": transaction.get("InvokeResult"),
    }


def dependency_delivery_class(transaction: Mapping[str, Any]) -> str:
    """Classify the causal block relation without comparing absolute heights."""
    if "OriginateHeight" not in transaction:
        return "source"

    origin_shard = transaction.get("OriginateShardIndex")
    destination_shard = transaction.get("ShardIndex")
    if origin_shard != destination_shard:
        # Heights from independent shard chains are not comparable. The
        # origin/destination shards remain part of the dependency class.
        return "cross_shard_or_scope"

    origin_height = transaction.get("OriginateHeight")
    destination_height = transaction.get("Height")
    if origin_height == destination_height:
        return "same_shard_same_block"
    if isinstance(origin_height, int) and isinstance(destination_height, int):
        if destination_height > origin_height:
            return "same_shard_later_block"
        return "same_shard_invalid_backedge"
    return "same_shard_unknown_block"


def _block_execution_order_key(block: Mapping[str, Any]) -> Tuple[str, int, str]:
    """Recover queue order within one destination without global ordering."""
    shard = canonical_json(block.get("ShardIndex"))
    height = block.get("Height")
    if isinstance(height, int) and not isinstance(height, bool):
        return shard, 0, "{:020d}".format(height)
    return shard, 1, canonical_json(height)


def _kitty_unordered_destination_batch(
    workload: str, transactions: Sequence[Mapping[str, Any]]
) -> bool:
    """Identify the one stock Kitty aggregation whose input order is unstable."""
    return bool(
        workload == "Kitty"
        and transactions
        and all(
            "registerNewBorns" in str(transaction.get("Function", ""))
            for transaction in transactions
        )
    )


def dependency_shape_projection(
    blocks: Sequence[Mapping[str, Any]], workload: str
) -> Dict[str, Any]:
    """Project causal block edges and order-preserving destination batches.

    Raw block heights and independent producer interleaving legitimately vary
    between asynchronous simulator processes. OriginateShardIndex/Height/Order
    still identify the real producer block inside one run, so they can recover
    each producer-to-destination dispatch batch. The absolute origin height is
    deliberately omitted from the canonical result after grouping.
    """
    causal_deliveries: List[Dict[str, Any]] = []
    producer_batches: Dict[
        Tuple[str, str, str, str], List[Dict[str, Any]]
    ] = {}
    producer_batch_metadata: Dict[
        Tuple[str, str, str, str], Dict[str, Any]
    ] = {}

    for block in sorted(blocks, key=_block_execution_order_key):
        confirmed = block.get("ConfirmTxn", [])
        if not isinstance(confirmed, list):
            confirmed = []
        for transaction in confirmed:
            if not isinstance(transaction, dict):
                continue
            causal_deliveries.append(
                {
                    "dependency": transaction_dependency_class(transaction),
                    "delivery": dependency_delivery_class(transaction),
                }
            )
            if "OriginateHeight" not in transaction:
                continue

            origin_shard = transaction.get("OriginateShardIndex")
            origin_height = transaction.get("OriginateHeight")
            origin_order = transaction.get("OriginateShardOrder")
            destination_shard = transaction.get("ShardIndex")
            batch_key = (
                canonical_json(origin_shard),
                canonical_json(origin_height),
                canonical_json(origin_order),
                canonical_json(destination_shard),
            )
            producer_batches.setdefault(batch_key, []).append(
                semantic_transaction(transaction, workload)
            )
            producer_batch_metadata.setdefault(
                batch_key,
                {
                    "origin_shard": origin_shard,
                    "origin_shard_order": origin_order,
                    "destination_shard": destination_shard,
                },
            )

    normalized_batches: List[Dict[str, Any]] = []
    for batch_key, transactions in producer_batches.items():
        metadata = dict(producer_batch_metadata[batch_key])
        if _kitty_unordered_destination_batch(workload, transactions):
            metadata["order_policy"] = "unordered_stock_aggregation"
            metadata["semantic_transactions"] = sorted_multiset(transactions)
        else:
            metadata["order_policy"] = "producer_order_preserved"
            metadata["semantic_transactions"] = transactions
        normalized_batches.append(metadata)

    return {
        "projection_version": 3,
        "causal_delivery_multiset": sorted_multiset(causal_deliveries),
        # Different producers may race legitimately, but one producer's
        # destination batch remains an ordered sequence.
        "producer_destination_batch_multiset": sorted_multiset(
            normalized_batches
        ),
    }


def diagnostics_projection(stdout_text: str, stderr_text: str) -> Dict[str, int]:
    lines = (ANSI_RE.sub("", stdout_text) + "\n" + ANSI_RE.sub("", stderr_text)).splitlines()
    selected: List[str] = []
    for line in lines:
        lowered = line.lower()
        if any(
            marker in lowered
            for marker in (
                "[error]",
                "invokeerror",
                "assertion failed",
                "segmentation fault",
                "exception",
            )
        ):
            normalized = re.sub(r"0x[0-9a-fA-F]+", "<hex>", line.strip())
            selected.append(normalized)
    return dict(sorted(collections.Counter(selected).items()))


def correctness_projection(record: Mapping[str, Any]) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    paths = record["paths"]
    viz_path = pathlib.Path(paths["viz"])
    viz_json = extract_viz_json(viz_path)
    blocks = block_records(viz_json)
    transactions: List[Dict[str, Any]] = []
    destinations: collections.Counter = collections.Counter()
    results: collections.Counter = collections.Counter()
    source_count = 0
    workload = str(record["workload"])

    for block in blocks:
        confirmed = block.get("ConfirmTxn", [])
        if not isinstance(confirmed, list):
            confirmed = []
        semantic = [
            semantic_transaction(transaction, workload)
            for transaction in confirmed
            if isinstance(transaction, dict)
        ]
        transactions.extend(semantic)
        for transaction in confirmed:
            if not isinstance(transaction, dict):
                continue
            if "OriginateShardIndex" in transaction:
                destinations[str(transaction.get("ShardIndex"))] += 1
            elif transaction.get("Contract") != "chain.deploy":
                source_count += 1
            results[str(transaction.get("InvokeResult", "<missing>"))] += 1

    state = state_projection(viz_json, workload)
    transactions = sorted_multiset(transactions)
    block_shapes = dependency_shape_projection(blocks, workload)
    optimization_report = record["optimization_report"] or {}
    counters = optimization_report.get("counters", {})
    stdout_text = pathlib.Path(paths["stdout"]).read_text(
        encoding="utf-8", errors="replace"
    )
    stderr_text = pathlib.Path(paths["stderr"]).read_text(
        encoding="utf-8", errors="replace"
    )
    diagnostics = diagnostics_projection(stdout_text, stderr_text)

    invariant = {
        "process_success": bool(record["run_succeeded"]),
        "final_contract_state_sha256": digest_json(state),
        "source_transaction_count": source_count,
        "logical_relay_count": counters.get("logical_relay_emissions"),
        "physical_route_count": counters.get("physical_relay_routes"),
        "relay_execution_count": counters.get("relay_executions"),
        "destination_shard_multiset": dict(sorted(destinations.items())),
        "confirmed_semantic_transaction_multiset_sha256": digest_json(transactions),
        "per_block_dependency_shape_sha256": digest_json(block_shapes),
        "broadcast_clone_count": counters.get("broadcast_physical_clones"),
        "invoke_result_multiset": dict(sorted(results.items())),
        "diagnostic_multiset": diagnostics,
    }
    normalized = {
        "schema_version": 2,
        "workload": record["workload"],
        "variant": record["variant"],
        "normalization_policy": CORRECTNESS_NORMALIZATION_POLICY,
        "invariant": invariant,
        "final_contract_state": state,
        "confirmed_semantic_transaction_multiset": transactions,
        "per_block_dependency_shape": block_shapes,
    }
    return invariant, normalized


def failed_correctness_projection(
    record: Mapping[str, Any],
) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    invariant = {field: None for field in CORRECTNESS_FIELDS}
    invariant["process_success"] = False
    normalized = {
        "schema_version": 1,
        "workload": record["workload"],
        "variant": record["variant"],
        "invariant": invariant,
        "run_failure": {
            "return_code": record.get("return_code"),
            "timed_out": record.get("timed_out"),
            "fatal_diagnostics": record.get("fatal_diagnostics", []),
            "feature_activation_issues": record.get(
                "feature_activation_issues", []
            ),
            "measurement_window_issue": record.get(
                "measurement_window_issue"
            ),
        },
    }
    return invariant, normalized


def compare_correctness(
    workload: str, variant_invariants: Mapping[str, Mapping[str, Any]]
) -> Dict[str, Any]:
    reference_variant = (
        "baseline"
        if "baseline" in variant_invariants
        else "scheduler_fifo"
    )
    baseline = variant_invariants.get(reference_variant)
    if baseline is None:
        raise HarnessError(
            "{} correctness has no reference variant".format(workload)
        )
    comparisons: Dict[str, Any] = {}
    overall = True
    for variant, actual in variant_invariants.items():
        mismatches = {}
        if actual.get("process_success") is not True:
            mismatches["process_success_required"] = {
                "expected": True,
                "actual": actual.get("process_success"),
            }
        for field in CORRECTNESS_FIELDS:
            if actual.get(field) != baseline.get(field):
                mismatches[field] = {
                    "baseline": baseline.get(field),
                    "actual": actual.get(field),
                }
        passed = not mismatches
        overall = overall and passed
        comparisons[variant] = {"passed": passed, "mismatches": mismatches}
    return {
        "workload": workload,
        "passed": overall,
        "reference_variant": reference_variant,
        "compared_fields": list(CORRECTNESS_FIELDS),
        "invariants": dict(variant_invariants),
        "comparisons": comparisons,
    }


def flatten_performance_record(record: Mapping[str, Any]) -> Dict[str, Any]:
    report = record.get("optimization_report") or {}
    counters, timings, derived, metrics_scope = selected_measurement_scope(report)
    simulator = record.get("simulator", {})
    resources = record.get("resources", {})
    return {
        "workload": record["workload"],
        "variant": record["variant"],
        "repetition": record["index"],
        "seed": record["seed"],
        "order": record["order"],
        "metrics_scope": metrics_scope,
        "measurement_window_status": record.get("measurement_window_status"),
        "configured_measured_source_transactions": record[
            "configured_measured_source_transactions"
        ],
        "stopwatch_elapsed_ms": simulator.get("stopwatch_elapsed_ms"),
        "source_tps": simulator.get("source_tps"),
        "utps": simulator.get("utps"),
        "wall_elapsed_seconds": record.get("wall_elapsed_seconds"),
        "max_rss_kib": resources.get("max_rss_kib"),
        "plan_loads": counters.get("plan_loads"),
        "plan_load_failures": counters.get("plan_load_failures"),
        "plan_binding_failures": counters.get("plan_binding_failures"),
        "plan_cache_hits": counters.get("plan_cache_hits"),
        "plan_cache_misses": counters.get("plan_cache_misses"),
        "plan_invalidations": counters.get("plan_invalidations"),
        "plan_reloads": counters.get("plan_reloads"),
        "optimization_eligible_lookups": counters.get(
            "optimization_eligible_lookups"
        ),
        "optimization_fallback_lookups": counters.get(
            "optimization_fallback_lookups"
        ),
        "optimization_eligible_invocations": counters.get(
            "optimization_eligible_invocations"
        ),
        "optimization_fallback_invocations": counters.get(
            "optimization_fallback_invocations"
        ),
        "scheduler_selection_calls": counters.get("scheduler_selection_calls"),
        "scheduler_priority_selections": counters.get(
            "scheduler_priority_selections"
        ),
        "scheduler_reorders": counters.get("scheduler_reorders"),
        "scheduler_certificate_uses": counters.get("scheduler_certificate_uses"),
        "scheduler_fallbacks": counters.get("scheduler_fallbacks"),
        "scheduler_decision_time_ns": timings.get("scheduler_decision_time_ns"),
        "relay_buffer_reserve_calls": counters.get(
            "relay_buffer_reserve_calls"
        ),
        "relay_buffer_reserved_elements": counters.get(
            "relay_buffer_reserved_elements"
        ),
        "relay_buffer_reserve_skipped_unknown": counters.get(
            "relay_buffer_reserve_skipped_unknown"
        ),
        "relay_buffer_reserve_skipped_limit": counters.get(
            "relay_buffer_reserve_skipped_limit"
        ),
        "relay_buffer_reserve_failures": counters.get(
            "relay_buffer_reserve_failures"
        ),
        "broadcast_clone_reserve_calls": counters.get(
            "broadcast_clone_reserve_calls"
        ),
        "relay_generation_time_ns": timings.get("relay_generation_time_ns"),
        "routing_time_ns": timings.get("routing_time_ns"),
        "dispatch_time_ns": timings.get("dispatch_time_ns"),
        "queue_push_time_ns": timings.get("queue_push_time_ns"),
        "plan_lookup_time_ns": timings.get("plan_lookup_time_ns"),
        "reserve_time_ns": timings.get("reserve_time_ns"),
        "queue_single_push_calls": counters.get("queue_single_push_calls"),
        "queue_legacy_bulk_push_calls": counters.get(
            "queue_legacy_bulk_push_calls"
        ),
        "queue_lock_acquisitions": counters.get("queue_lock_acquisitions"),
        "queue_batch_push_calls": counters.get("queue_batch_push_calls"),
        "queue_batch_elements": counters.get("queue_batch_elements"),
        "maximum_batch_size": counters.get("maximum_batch_size"),
        "queue_batch_fallbacks": counters.get("queue_batch_fallbacks"),
        "queue_notifications": counters.get("queue_notifications"),
        "average_batch_size": derived.get("average_batch_size"),
        "relay_buffer_capacity_growth_events": counters.get(
            "relay_buffer_capacity_growth_events"
        ),
        "relay_buffer_capacity_misses": counters.get(
            "relay_buffer_capacity_misses"
        ),
        "logical_relay_emissions": counters.get("logical_relay_emissions"),
        "physical_relay_routes": counters.get("physical_relay_routes"),
        "relay_executions": counters.get("relay_executions"),
        "broadcast_physical_clones": counters.get("broadcast_physical_clones"),
        "run_succeeded": record.get("run_succeeded"),
        "run_directory": record["paths"]["run_directory"],
    }


def stats(values: Sequence[float]) -> Dict[str, Any]:
    return {
        "n": len(values),
        "raw_samples": list(values),
        "median": statistics.median(values),
        "mean": float(sum(values)) / len(values),
        "min": min(values),
        "max": max(values),
        "range": max(values) - min(values),
        "population_standard_deviation": statistics.pstdev(values),
    }


def build_performance_summary(
    samples: Sequence[Mapping[str, Any]], required_samples: int
) -> Dict[str, Any]:
    grouped: Dict[Tuple[str, str], List[Mapping[str, Any]]] = collections.defaultdict(list)
    for sample in samples:
        grouped[(sample["workload"], sample["variant"])].append(sample)
    result: Dict[str, Any] = {}
    for (workload, variant), group in sorted(grouped.items()):
        successful_group = [item for item in group if item.get("run_succeeded")]
        metrics: Dict[str, Any] = {}
        for field in PERFORMANCE_FIELDS:
            values = [
                sample[field]
                for sample in successful_group
                if sample.get(field) is not None
            ]
            if values:
                metrics[field] = stats(values)
        result.setdefault(workload, {})[variant] = {
            "complete": (
                len(group) == required_samples
                and len(successful_group) == required_samples
            ),
            "required_samples": required_samples,
            "successful_samples": len(successful_group),
            "failed_samples": len(group) - len(successful_group),
            "total_samples": len(group),
            "metrics": metrics,
        }
    return {"schema_version": 1, "workloads": result}


def performance_completion_issues(
    samples: Sequence[Mapping[str, Any]],
    workload_names: Sequence[str],
    variants: Sequence[str],
    repetitions: int,
) -> List[str]:
    grouped: Dict[Tuple[str, str], List[Mapping[str, Any]]] = (
        collections.defaultdict(list)
    )
    for sample in samples:
        grouped[(sample["workload"], sample["variant"])].append(sample)

    issues: List[str] = []
    for workload in workload_names:
        for variant in variants:
            group = grouped.get((workload, variant), [])
            successful = sum(bool(item.get("run_succeeded")) for item in group)
            if len(group) != repetitions or successful != repetitions:
                issues.append(
                    "{} {} requires {} successful samples; observed {} total, "
                    "{} successful".format(
                        workload,
                        variant,
                        repetitions,
                        len(group),
                        successful,
                    )
                )
    return issues


def write_samples_csv(path: pathlib.Path, samples: Sequence[Mapping[str, Any]]) -> None:
    fields = [
        "workload",
        "variant",
        "repetition",
        "schedule_index",
        "seed",
        "order",
        "metrics_scope",
        "measurement_window_status",
        "configured_measured_source_transactions",
    ] + list(PERFORMANCE_FIELDS) + ["run_succeeded", "run_directory"]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(samples)


def write_summary_csv(path: pathlib.Path, summary: Mapping[str, Any]) -> None:
    fields = (
        "workload",
        "variant",
        "metric",
        "n",
        "median",
        "mean",
        "min",
        "max",
        "range",
        "population_standard_deviation",
        "raw_samples_json",
    )
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for workload, variants in summary.get("workloads", {}).items():
            for variant, variant_summary in variants.items():
                for metric, metric_summary in variant_summary["metrics"].items():
                    row = {
                        "workload": workload,
                        "variant": variant,
                        "metric": metric,
                        "raw_samples_json": json.dumps(
                            metric_summary["raw_samples"], separators=(",", ":")
                        ),
                    }
                    row.update(
                        {
                            key: metric_summary[key]
                            for key in fields
                            if key in metric_summary
                        }
                    )
                    writer.writerow(row)


def make_schedule(
    workload_names: Sequence[str],
    variants: Sequence[str],
    repetitions: int,
    seed: int,
) -> List[Tuple[str, str, int]]:
    schedule: List[Tuple[str, str, int]] = []
    randomizer = random.Random(seed)
    for workload in workload_names:
        for repetition in range(repetitions):
            order = list(variants)
            randomizer.shuffle(order)
            schedule.extend((workload, variant, repetition) for variant in order)
    return schedule


def validate_tools(args: argparse.Namespace, enforce_binary_mode: bool) -> None:
    if not args.binary.is_file() or not os.access(str(args.binary), os.X_OK):
        raise HarnessError("simulator binary is missing or not executable: {}".format(args.binary))
    if not VIZ_TEMPLATE.is_file():
        raise HarnessError("missing visualization template: {}".format(VIZ_TEMPLATE))
    if args.time_binary is not None and (
        not args.time_binary.is_file() or not os.access(str(args.time_binary), os.X_OK)
    ):
        raise HarnessError(
            "GNU time binary is missing or not executable: {}".format(
                args.time_binary
            )
        )
    if args.cpu_list and shutil.which("taskset") is None:
        raise HarnessError("--cpu-list requires taskset")
    if args.toolchain_bin is not None and not args.toolchain_bin.is_dir():
        raise HarnessError("toolchain bin directory is missing: {}".format(args.toolchain_bin))
    if enforce_binary_mode:
        binary_data = args.binary.read_bytes()
        if b"[R-PREDA optimization]: mode=" not in binary_data:
            raise HarnessError(
                "binary does not contain the runtime-optimization path; rebuild with "
                "RPREDA_ENABLE_RUNTIME_OPTIMIZATION=ON"
            )
        if b"[R-PREDA trace]: mode=" in binary_data:
            raise HarnessError(
                "binary contains the full runtime-trace path; benchmark acceptance "
                "requires RPREDA_ENABLE_RUNTIME_TRACE=OFF"
            )


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, default=DEFAULT_BINARY)
    parser.add_argument(
        "--toolchain-bin",
        type=pathlib.Path,
        help=(
            "compiler toolchain bin used by runtime contract compilation; "
            "defaults to RPREDA_TOOLCHAIN_BIN or build-gcc12/CMakeCache.txt"
        ),
    )
    parser.add_argument("--config", type=pathlib.Path, default=DEFAULT_CONFIG)
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path(
            "/tmp/rpreda-native-ab-{}".format(dt.datetime.now().strftime("%Y%m%d-%H%M%S"))
        ),
    )
    parser.add_argument(
        "--phase", choices=("all", "correctness", "performance"), default="all"
    )
    parser.add_argument("--workload", action="append", default=[])
    parser.add_argument("--variant", action="append", default=[])
    parser.add_argument("--performance-parameter", action="append", default=[])
    parser.add_argument("--correctness-parameter", action="append", default=[])
    parser.add_argument("--seed", type=int)
    parser.add_argument("--order", type=int)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--schedule-seed", type=int, default=20260730)
    parser.add_argument("--timeout-seconds", type=float, default=1800.0)
    parser.add_argument("--cpu-list", help="optional taskset CPU list, for example 0-7")
    parser.add_argument(
        "--time-binary",
        type=pathlib.Path,
        default=None,
        help=(
            "optional GNU time executable; without it peak RSS is sampled from "
            "/proc for the simulator process tree"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--allow-correctness-mismatch", action="store_true")
    args = parser.parse_args(argv)
    args.binary = args.binary.resolve()
    args.toolchain_bin = (
        args.toolchain_bin.resolve()
        if args.toolchain_bin is not None
        else detect_toolchain_bin()
    )
    args.config = args.config.resolve()
    args.output = args.output.resolve()
    if args.time_binary is not None:
        args.time_binary = args.time_binary.resolve()
    if args.warmups < 0 or args.repetitions < 2:
        parser.error(
            "warmups must be >= 0 and repetitions must be >= 2 for "
            "multi-run A/B statistics"
        )
    if args.timeout_seconds <= 0:
        parser.error("timeout must be positive")
    return args


def print_dry_run(
    args: argparse.Namespace,
    workloads: Sequence[Mapping[str, Any]],
    variants: Sequence[str],
) -> None:
    print("binary:", args.binary)
    print("output:", args.output)
    print("phase:", args.phase)
    print("variants:", ", ".join(variants))
    for workload in workloads:
        print("{}:".format(workload["name"]))
        print("  dataset/case:", workload.get("dataset"), workload.get("case"))
        if workload.get("source_contract"):
            print("  source contract:", workload["source_contract"])
        print("  correctness fixture:", workload["correctness_fixture"])
        print("  performance fixture:", workload["performance_fixture"])
        print("  correctness:", workload["correctness_parameters"])
        print("  performance:", workload["performance_parameters"])
        preview = command_for_run(
            args.binary,
            workload["performance_fixture"],
            variants[0],
            workload["performance_parameters"],
            args.seed,
            args.order,
            pathlib.Path("<report.json>"),
            None,
        )
        print("  command:", " ".join(preview))


def record_run_failure(session: Dict[str, Any], record: Mapping[str, Any]) -> None:
    session.setdefault("run_failures", []).append(
        {
            "workload": record.get("workload"),
            "variant": record.get("variant"),
            "phase": record.get("phase"),
            "index": record.get("index"),
            "run_directory": record.get("paths", {}).get("run_directory"),
            "return_code": record.get("return_code"),
            "timed_out": record.get("timed_out"),
            "fatal_diagnostics": record.get("fatal_diagnostics", []),
            "feature_activation_issues": record.get(
                "feature_activation_issues", []
            ),
            "measurement_window_issue": record.get(
                "measurement_window_issue"
            ),
        }
    )


def _run_harness(args: argparse.Namespace) -> int:
    if not args.config.is_file():
        raise HarnessError("missing workload config: {}".format(args.config))
    performance_overrides = parse_parameter_overrides(args.performance_parameter)
    correctness_overrides = parse_parameter_overrides(args.correctness_parameter)
    config, workloads = load_workloads(
        args.config,
        args.workload,
        performance_overrides,
        correctness_overrides,
    )
    variants = select_variants(args.variant)
    defaults = config.get("defaults", {})
    if args.seed is None:
        args.seed = int(defaults.get("seed", 88))
    if args.order is None:
        args.order = int(defaults.get("order", 2))
    if args.seed < 0 or args.order <= 0:
        raise HarnessError("seed must be non-negative and order must be positive")
    validate_tools(args, enforce_binary_mode=not args.dry_run)

    if args.dry_run:
        print_dry_run(args, workloads, variants)
        return 0
    if args.output.exists() and any(args.output.iterdir()):
        raise HarnessError("output directory is not empty: {}".format(args.output))
    args.output.mkdir(parents=True, exist_ok=True)

    session: Dict[str, Any] = {
        "schema_version": 1,
        "status": "running",
        "started_at": utc_now(),
        "binary": str(args.binary),
        "binary_sha256": sha256_file(args.binary),
        "toolchain_bin": str(args.toolchain_bin) if args.toolchain_bin else None,
        "config": str(args.config),
        "config_sha256": sha256_file(args.config),
        "phase": args.phase,
        "seed": args.seed,
        "order": args.order,
        "warmups": args.warmups,
        "repetitions": args.repetitions,
        "schedule_seed": args.schedule_seed,
        "cpu_list": args.cpu_list,
        "trace_required": "disabled",
        "workloads": [
            {
                key: (str(value) if isinstance(value, pathlib.Path) else value)
                for key, value in workload.items()
            }
            for workload in workloads
        ],
        "variants": list(variants),
        "run_failures": [],
        "host": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "processor": platform.processor(),
            "cpu_count": os.cpu_count(),
        },
        "git": git_metadata(),
        "correctness_normalization_policy": CORRECTNESS_NORMALIZATION_POLICY,
        "notes": [
            "Correctness fixtures issue state/block viz queries; timing fixtures do not.",
            "Performance runs do not enable visualization.",
            "Stopwatch metrics cover the fixture's marked transaction window.",
            "Performance requires a completed schema-v2 measurement_window; "
            "schema-v1 lifetime reports are rejected for timing acceptance.",
            "Peak RSS covers the full simulator process.",
            "Warmup samples are retained under raw/ but excluded from statistics.",
            "Correctness retains causal delivery classes and ordered "
            "producer-to-destination batches without imposing an independent "
            "cross-producer or cross-destination total order.",
            "Kitty correctness removes scheduling-assigned birth/ID fields and "
            "canonicalizes documented unordered collections, including the "
            "registerNewBorns aggregation-derived destination batches.",
        ],
    }
    write_json(args.output / "session.json", session)
    args._session_owned = True
    workload_lookup = {workload["name"]: workload for workload in workloads}

    correctness_results: Dict[str, Any] = {}
    if args.phase in {"all", "correctness"}:
        for workload in workloads:
            variant_invariants: Dict[str, Any] = {}
            for variant_index, variant in enumerate(variants):
                run_dir = (
                    args.output
                    / "raw"
                    / workload["name"]
                    / variant
                    / "correctness"
                )
                print(
                    "[correctness] {} {}".format(workload["name"], variant),
                    flush=True,
                )
                record = execute_run(
                    args,
                    workload,
                    variant,
                    "correctness",
                    variant_index,
                    workload["correctness_parameters"],
                    run_dir,
                    collect_viz=True,
                )
                if not record["run_succeeded"]:
                    record_run_failure(session, record)
                    invariant, normalized = failed_correctness_projection(record)
                    write_json(args.output / "session.json", session)
                else:
                    invariant, normalized = correctness_projection(record)
                write_json(run_dir / "correctness.normalized.json", normalized)
                variant_invariants[variant] = invariant
                if not record["run_succeeded"] and not args.keep_going:
                    raise HarnessError(
                        "correctness run failed: {} {}".format(
                            workload["name"], variant
                        )
                    )
            correctness_results[workload["name"]] = compare_correctness(
                workload["name"], variant_invariants
            )
            write_json(args.output / "correctness.json", {
                "schema_version": 1,
                "passed": all(item["passed"] for item in correctness_results.values()),
                "workloads": correctness_results,
            })
        correctness_passed = all(item["passed"] for item in correctness_results.values())
        if not correctness_passed and not args.allow_correctness_mismatch:
            raise HarnessError(
                "correctness invariance failed; performance phase was not started"
            )

    performance_samples: List[Dict[str, Any]] = []
    if args.phase in {"all", "performance"}:
        # Warm each workload/variant independently.  Each run still receives a
        # fresh HOME, preventing module/chain data from leaking across modes.
        for workload in workloads:
            for variant in variants:
                for warmup in range(args.warmups):
                    run_dir = (
                        args.output
                        / "raw"
                        / workload["name"]
                        / variant
                        / "warmup-{:03d}".format(warmup)
                    )
                    print(
                        "[warmup] {} {} {}/{}".format(
                            workload["name"], variant, warmup + 1, args.warmups
                        ),
                        flush=True,
                    )
                    record = execute_run(
                        args,
                        workload,
                        variant,
                        "warmup",
                        warmup,
                        workload["performance_parameters"],
                        run_dir,
                        collect_viz=False,
                    )
                    if not record["run_succeeded"]:
                        record_run_failure(session, record)
                        write_json(args.output / "session.json", session)
                        if not args.keep_going:
                            raise HarnessError(
                                "warmup failed: {} {}".format(
                                    workload["name"], variant
                                )
                            )

        schedule = make_schedule(
            [workload["name"] for workload in workloads],
            variants,
            args.repetitions,
            args.schedule_seed,
        )
        session["measured_schedule"] = [
            {"workload": workload, "variant": variant, "repetition": repetition}
            for workload, variant, repetition in schedule
        ]
        write_json(args.output / "session.json", session)
        for schedule_index, (workload_name, variant, repetition) in enumerate(schedule):
            workload = workload_lookup[workload_name]
            run_dir = (
                args.output
                / "raw"
                / workload_name
                / variant
                / "sample-{:03d}".format(repetition)
            )
            print(
                "[sample {}/{}] {} {} repetition {}".format(
                    schedule_index + 1,
                    len(schedule),
                    workload_name,
                    variant,
                    repetition,
                ),
                flush=True,
            )
            record = execute_run(
                args,
                workload,
                variant,
                "sample",
                repetition,
                workload["performance_parameters"],
                run_dir,
                collect_viz=False,
            )
            sample = flatten_performance_record(record)
            sample["schedule_index"] = schedule_index
            performance_samples.append(sample)
            write_samples_csv(args.output / "samples.csv", performance_samples)
            summary = build_performance_summary(
                performance_samples, args.repetitions
            )
            write_json(args.output / "summary.json", summary)
            write_summary_csv(args.output / "summary.csv", summary)
            if not record["run_succeeded"]:
                record_run_failure(session, record)
                write_json(args.output / "session.json", session)
                if not args.keep_going:
                    raise HarnessError(
                        "performance run failed: {} {}".format(
                            workload_name, variant
                        )
                    )

        completion_issues = performance_completion_issues(
            performance_samples,
            [workload["name"] for workload in workloads],
            variants,
            args.repetitions,
        )
        session["performance_completion_issues"] = completion_issues
        write_json(args.output / "session.json", session)
        if completion_issues:
            raise HarnessError(
                "performance sampling incomplete: {}".format(
                    "; ".join(completion_issues)
                )
            )

    if session["run_failures"]:
        raise HarnessError(
            "{} benchmark run(s) failed; see session.json".format(
                len(session["run_failures"])
            )
        )

    session["status"] = "complete"
    session["completed_at"] = utc_now()
    session["correctness_passed"] = (
        all(item["passed"] for item in correctness_results.values())
        if correctness_results
        else None
    )
    session["measured_sample_count"] = len(performance_samples)
    write_json(args.output / "session.json", session)
    print("results:", args.output)
    return 0


def mark_owned_session_failed(
    args: argparse.Namespace, error: BaseException
) -> None:
    if not getattr(args, "_session_owned", False):
        return
    path = args.output / "session.json"
    try:
        session = read_json(path)
        if not isinstance(session, dict):
            return
        session["status"] = "failed"
        session["completed_at"] = utc_now()
        session["failure_type"] = type(error).__name__
        session["failure_reason"] = str(error)
        write_json(path, session)
    except Exception as finalization_error:
        print(
            "warning: failed to finalize session status: {}".format(
                finalization_error
            ),
            file=sys.stderr,
        )


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    args._session_owned = False
    try:
        return _run_harness(args)
    except Exception as error:
        mark_owned_session_failed(args, error)
        raise


if __name__ == "__main__":
    try:
        sys.exit(main())
    except HarnessError as error:
        print("error: {}".format(error), file=sys.stderr)
        sys.exit(1)
