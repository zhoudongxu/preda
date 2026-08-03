#!/usr/bin/env python3
"""Run the standalone PREDA analysis driver in fresh measured processes.

The runner intentionally measures peak RSS outside the compiler.  On Linux it
uses wait4(2), so no production compiler or runtime code needs a memory probe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, Sequence


@dataclass
class MeasuredProcess:
    returncode: int | None
    timed_out: bool
    wall_time_ms: float
    peak_rss_kib: int | None
    launch_error: str = ""


def _write_json(path: pathlib.Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _normalize_peak_rss(value: int) -> int:
    # Linux reports KiB; macOS reports bytes.
    return int(math.ceil(value / 1024.0)) if sys.platform == "darwin" else int(value)


def _wait_status_to_exitcode(status: int) -> int:
    converter = getattr(os, "waitstatus_to_exitcode", None)
    if converter is not None:
        return int(converter(status))
    if os.WIFEXITED(status):
        return int(os.WEXITSTATUS(status))
    if os.WIFSIGNALED(status):
        return -int(os.WTERMSIG(status))
    if os.WIFSTOPPED(status):
        return -int(os.WSTOPSIG(status))
    raise RuntimeError(f"unrecognized wait status: {status}")


def _wait4_process(
    command: Sequence[str],
    cwd: pathlib.Path,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    timeout_seconds: float,
) -> MeasuredProcess:
    started = time.monotonic()
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr:
            process = subprocess.Popen(
                list(command),
                cwd=str(cwd),
                env=dict(os.environ),
                stdout=stdout,
                stderr=stderr,
                text=True,
                start_new_session=True,
            )
            timed_out = False
            termination_started: float | None = None
            kill_sent = False
            usage = None
            status = None
            while True:
                waited_pid, candidate_status, candidate_usage = os.wait4(
                    process.pid, os.WNOHANG
                )
                if waited_pid == process.pid:
                    status = candidate_status
                    usage = candidate_usage
                    break
                now = time.monotonic()
                if not timed_out and now - started >= timeout_seconds:
                    timed_out = True
                    termination_started = now
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                elif (
                    timed_out
                    and termination_started is not None
                    and now - termination_started >= 5.0
                    and not kill_sent
                ):
                    kill_sent = True
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                time.sleep(0.01)
            returncode = _wait_status_to_exitcode(status)
            # Mark the Popen object as reaped; its destructor must not wait a
            # second time after wait4 supplied the per-child rusage.
            process.returncode = returncode
            peak = _normalize_peak_rss(usage.ru_maxrss) if usage is not None else None
            return MeasuredProcess(
                returncode,
                timed_out,
                (time.monotonic() - started) * 1000.0,
                peak,
            )
    except OSError as exc:
        return MeasuredProcess(
            None,
            False,
            (time.monotonic() - started) * 1000.0,
            None,
            str(exc),
        )


def _portable_process(
    command: Sequence[str],
    cwd: pathlib.Path,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    timeout_seconds: float,
) -> MeasuredProcess:
    started = time.monotonic()
    try:
        with stdout_path.open("w", encoding="utf-8") as stdout, stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr:
            completed = subprocess.run(
                list(command),
                cwd=str(cwd),
                env=dict(os.environ),
                stdout=stdout,
                stderr=stderr,
                text=True,
                timeout=timeout_seconds,
                check=False,
            )
        return MeasuredProcess(
            completed.returncode,
            False,
            (time.monotonic() - started) * 1000.0,
            None,
        )
    except subprocess.TimeoutExpired:
        return MeasuredProcess(
            None,
            True,
            (time.monotonic() - started) * 1000.0,
            None,
        )
    except OSError as exc:
        return MeasuredProcess(
            None,
            False,
            (time.monotonic() - started) * 1000.0,
            None,
            str(exc),
        )


def run_measured(
    command: Sequence[str],
    cwd: pathlib.Path,
    stdout_path: pathlib.Path,
    stderr_path: pathlib.Path,
    timeout_seconds: float,
) -> MeasuredProcess:
    if hasattr(os, "wait4"):
        return _wait4_process(
            command, cwd, stdout_path, stderr_path, timeout_seconds
        )
    return _portable_process(
        command, cwd, stdout_path, stderr_path, timeout_seconds
    )


def _percentile95(values: Sequence[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return float(ordered[rank])


def _median(values: Sequence[float]) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    middle = len(ordered) // 2
    if len(ordered) % 2:
        return float(ordered[middle])
    return float((ordered[middle - 1] + ordered[middle]) / 2.0)


def _distribution(values: Sequence[float]) -> Mapping[str, float | int | None]:
    return {
        "samples": len(values),
        "minimum": min(values) if values else None,
        "median": _median(values),
        "p95": _percentile95(values),
        "maximum": max(values) if values else None,
    }


def run_samples(
    driver: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    dapp: str,
    warmups: int,
    repetitions: int,
    timeout_seconds: float,
) -> Mapping[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    records: List[Dict[str, Any]] = []
    for index in range(warmups + repetitions):
        warmup = index < warmups
        measured_index = index - warmups if not warmup else index
        label = (
            f"warmup_{measured_index:03d}"
            if warmup
            else f"sample_{measured_index:03d}"
        )
        directory = output / label
        directory.mkdir(parents=True, exist_ok=True)
        manifest = directory / "manifest.json"
        metrics = directory / "metrics.json"
        command = [
            str(driver),
            "--source",
            str(source),
            "--manifest",
            str(manifest),
            "--metrics",
            str(metrics),
            "--dapp",
            dapp,
            "--profile",
            "on",
        ]
        process = run_measured(
            command,
            driver.parent,
            directory / "stdout.log",
            directory / "stderr.log",
            timeout_seconds,
        )
        compiler_metrics: Mapping[str, Any] = {}
        if metrics.is_file():
            try:
                compiler_metrics = json.loads(metrics.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                compiler_metrics = {}
        success = (
            process.returncode == 0
            and not process.timed_out
            and not process.launch_error
            and compiler_metrics.get("status") == "Compiled"
            and manifest.is_file()
        )
        records.append(
            {
                "sample_id": label,
                "warmup": warmup,
                "success": success,
                "returncode": process.returncode,
                "timed_out": process.timed_out,
                "launch_error": process.launch_error,
                "wall_time_ms": process.wall_time_ms,
                "peak_rss_kib": process.peak_rss_kib,
                "peak_rss_measurement": (
                    "wait4_ru_maxrss" if process.peak_rss_kib is not None else "unavailable"
                ),
                "manifest_size_bytes": manifest.stat().st_size if manifest.is_file() else None,
                "metrics": compiler_metrics,
                "artifacts": {
                    "manifest": f"{label}/manifest.json",
                    "metrics": f"{label}/metrics.json",
                    "stdout": f"{label}/stdout.log",
                    "stderr": f"{label}/stderr.log",
                },
            }
        )

    measured = [record for record in records if not record["warmup"]]
    successful = [record for record in measured if record["success"]]
    phase_names = sorted(
        {
            str(name)
            for record in successful
            for name in record["metrics"].get("phase_times_ms", {})
        }
    )
    phase_distributions = {
        name: _distribution(
            [
                float(record["metrics"]["phase_times_ms"][name])
                for record in successful
                if name in record["metrics"].get("phase_times_ms", {})
            ]
        )
        for name in phase_names
    }
    aggregate = {
        "requested_samples": repetitions,
        "successful_samples": len(successful),
        "wall_time_ms": _distribution(
            [float(record["wall_time_ms"]) for record in successful]
        ),
        "peak_rss_kib": _distribution(
            [
                float(record["peak_rss_kib"])
                for record in successful
                if record["peak_rss_kib"] is not None
            ]
        ),
        "manifest_size_bytes": _distribution(
            [float(record["manifest_size_bytes"]) for record in successful]
        ),
        "total_analysis_ms": _distribution(
            [
                float(record["metrics"].get("total_analysis_ms", 0.0))
                for record in successful
            ]
        ),
        "phase_times_ms": phase_distributions,
    }
    return {
        "schema_version": 1,
        "status": (
            "Completed"
            if len(successful) == repetitions
            else "InfrastructureFailure"
        ),
        "source_file_name": source.name,
        "source_sha256": _sha256(source),
        "driver_file_name": driver.name,
        "driver_sha256": _sha256(driver),
        "dapp": dapp,
        "warmups": warmups,
        "repetitions": repetitions,
        "timeout_seconds": timeout_seconds,
        "samples": records,
        "aggregate": aggregate,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--dapp", default="RPredaScalability")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    args = parser.parse_args(argv)
    driver = args.driver.expanduser().resolve()
    source = args.source.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not driver.is_file():
        parser.error(f"analysis driver does not exist: {driver}")
    if not source.is_file():
        parser.error(f"source does not exist: {source}")
    if args.warmups < 0 or args.repetitions <= 0:
        parser.error("warmups must be nonnegative and repetitions must be positive")
    if args.timeout_seconds <= 0:
        parser.error("timeout must be positive")
    payload = run_samples(
        driver,
        source,
        output,
        args.dapp,
        args.warmups,
        args.repetitions,
        args.timeout_seconds,
    )
    _write_json(output / "analysis_samples.json", payload)
    return 0 if payload["status"] == "Completed" else 2


if __name__ == "__main__":
    raise SystemExit(main())
