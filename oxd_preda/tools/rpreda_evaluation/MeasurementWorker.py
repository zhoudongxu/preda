#!/usr/bin/env python3
"""Run exactly one analysis process and report per-process resource usage."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import resource
import signal
import subprocess
import sys
import time
from typing import Any, Dict, Sequence


def main(argv: Sequence[str] = ()) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=120.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(list(argv) if argv else None)
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a command is required after --")
    started = time.monotonic()
    try:
        process = subprocess.Popen(
            command,
            cwd=args.cwd,
            env=dict(os.environ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        timed_out = False
        try:
            stdout, stderr = process.communicate(timeout=args.timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                stdout, stderr = process.communicate(timeout=2.0)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                stdout, stderr = process.communicate()
        returncode = process.returncode
        launch_error = ""
    except OSError as exc:
        stdout, stderr, returncode, launch_error, timed_out = "", "", None, str(exc), False
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    payload: Dict[str, Any] = {
        "schema_version": 1,
        "command": command,
        "returncode": returncode,
        "launch_error": launch_error,
        "timed_out": timed_out,
        "wall_time_ms": round((time.monotonic() - started) * 1000.0, 6),
        "peak_rss_kib": int(usage.ru_maxrss),
        "user_cpu_ms": round(float(usage.ru_utime) * 1000.0, 6),
        "system_cpu_ms": round(float(usage.ru_stime) * 1000.0, 6),
        "stdout": stdout,
        "stderr": stderr,
    }
    pathlib.Path(args.output).write_text(
        json.dumps(payload, sort_keys=True, indent=2) + "\n", encoding="utf-8"
    )
    return 0 if returncode == 0 and not launch_error and not timed_out else 1


if __name__ == "__main__":
    raise SystemExit(main())
