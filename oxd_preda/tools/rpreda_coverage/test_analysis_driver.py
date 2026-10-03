#!/usr/bin/env python3
"""Integration checks for the opt-in compiler analysis profiler."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import tempfile

import AnalysisProcessRunner


EXPECTED_PHASES = {
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


def _run_driver(
    driver: pathlib.Path,
    fixture: pathlib.Path,
    directory: pathlib.Path,
    profile: str,
    analysis_mode: str = "full",
) -> tuple[dict, bytes, dict]:
    manifest = directory / "manifest.json"
    metrics = directory / "metrics.json"
    generated = directory / "generated.cpp"
    completed = subprocess.run(
        [
            str(driver),
            "--source",
            str(fixture),
            "--manifest",
            str(manifest),
            "--metrics",
            str(metrics),
            "--generated-cpp",
            str(generated),
            "--dapp",
            "RPredaAnalysisDriverTest",
            "--profile",
            profile,
            "--analysis-mode",
            analysis_mode,
        ],
        cwd=str(driver.parent),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"driver failed with {completed.returncode}: {completed.stderr}"
        )
    payload = json.loads(metrics.read_text(encoding="utf-8"))
    manifest_payload = json.loads(manifest.read_text(encoding="utf-8"))
    return payload, generated.read_bytes(), manifest_payload


def _without_existing_solver_timings(value):
    if isinstance(value, dict):
        return {
            key: _without_existing_solver_timings(child)
            for key, child in value.items()
            if key != "elapsed_time_ms"
        }
    if isinstance(value, list):
        return [_without_existing_solver_timings(child) for child in value]
    return value


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument(
        "--runtime-trace-enabled", type=int, choices=(0, 1), required=True
    )
    parser.add_argument(
        "--runtime-optimization-enabled", type=int, choices=(0, 1), required=True
    )
    parser.add_argument("--z3-enabled", type=int, choices=(0, 1), required=True)
    parser.add_argument(
        "--bound-manifest-enabled", type=int, choices=(0, 1), required=True
    )
    args = parser.parse_args()
    driver = args.driver.resolve()
    fixture = args.fixture.resolve()

    with tempfile.TemporaryDirectory(prefix="rpreda-analysis-driver-") as raw:
        root = pathlib.Path(raw)
        enabled_dir = root / "enabled"
        disabled_dir = root / "disabled"
        enabled_dir.mkdir()
        disabled_dir.mkdir()
        enabled, enabled_cpp, enabled_manifest = _run_driver(
            driver, fixture, enabled_dir, "on"
        )
        disabled, disabled_cpp, disabled_manifest = _run_driver(
            driver, fixture, disabled_dir, "off"
        )

        assert enabled["status"] == "Compiled"
        assert enabled["profiling_requested"] is True
        assert enabled["profiling_enabled"] is True
        assert enabled["build_features"] == {
            "analysis_profiling": True,
            "runtime_trace": bool(args.runtime_trace_enabled),
            "runtime_optimization": bool(args.runtime_optimization_enabled),
            "z3": bool(args.z3_enabled),
            "bound_manifest": bool(args.bound_manifest_enabled),
        }
        assert set(enabled["phase_times_ms"]) == EXPECTED_PHASES
        assert set(enabled["analysis_phases"]) == EXPECTED_PHASES
        assert isinstance(enabled["total_analysis_ms"], (int, float))
        assert enabled_manifest["schema_version"] == 5
        assert enabled_manifest["control_flow"]["extension_schema_version"] == 1
        assert (
            enabled_manifest["parallel_certificate"]["extension_schema_version"]
            == 1
        )
        assert all("ordinal" in site for site in enabled_manifest["relay_sites"])
        for phase in (
            "cfg_construction",
            "call_graph_construction",
            "effect_analysis",
            "icfg_construction",
            "summary_analysis",
            "refinement_generation",
            "refinement_total",
            "certificate_generation",
            "analysis_total",
            "manifest_emission",
        ):
            assert enabled["analysis_phases"][phase]["invocations"] >= 0, phase

        assert disabled["status"] == "Compiled"
        assert disabled["profiling_requested"] is False
        assert disabled["profiling_enabled"] is False
        assert disabled["build_features"] == enabled["build_features"]
        assert disabled["total_analysis_ms"] == 0
        assert all(value == 0 for value in disabled["phase_times_ms"].values())
        assert all(
            phase["invocations"] == 0
            for phase in disabled["analysis_phases"].values()
        )

        # Profiling is observation-only: neither lowering nor manifest content
        # can depend on whether the timers were enabled.
        assert enabled_cpp == disabled_cpp
        assert b"prlrt::relay(" in enabled_cpp
        if not args.runtime_trace_enabled:
            assert b"_RPredaRuntimeTraceAbiVersion" not in enabled_cpp
            assert b"_CreateInstance_RPredaTraceV1" not in enabled_cpp
        assert "phase_times_ms" not in enabled_manifest
        assert "analysis_phases" not in enabled_manifest
        assert _without_existing_solver_timings(
            enabled_manifest
        ) == _without_existing_solver_timings(disabled_manifest)

        # Every ablation mode must preserve generated code.  The mode is an
        # analysis-only switch, and lower layers must explicitly report which
        # stage was selected.
        mode_outputs = {}
        for mode in ("site_scan", "cfg_icfg", "formula_smt", "full"):
            mode_dir = root / mode
            mode_dir.mkdir()
            metrics, generated_mode, _ = _run_driver(
                driver, fixture, mode_dir, "on", mode
            )
            assert metrics["status"] == "Compiled"
            assert metrics["analysis_mode"] == mode
            assert generated_mode == enabled_cpp
            mode_outputs[mode] = metrics
        assert mode_outputs["site_scan"]["analysis_phases"]["refinement_solver"]["invocations"] == 0
        assert mode_outputs["cfg_icfg"]["analysis_phases"]["refinement_solver"]["invocations"] == 0
        assert mode_outputs["formula_smt"]["analysis_phases"]["certificate_generation"]["invocations"] == 0
        assert mode_outputs["full"]["analysis_phases"]["certificate_generation"]["invocations"] > 0

        sampled = AnalysisProcessRunner.run_samples(
            driver=driver,
            source=fixture,
            output=root / "process_runner",
            dapp="RPredaAnalysisDriverRunnerTest",
            warmups=0,
            repetitions=1,
            timeout_seconds=120.0,
        )
        assert sampled["status"] == "Completed"
        assert sampled["aggregate"]["successful_samples"] == 1
        if hasattr(os, "wait4"):
            assert sampled["samples"][0]["peak_rss_kib"] is not None
            assert sampled["samples"][0]["peak_rss_kib"] > 0

    print("rpreda analysis driver tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
