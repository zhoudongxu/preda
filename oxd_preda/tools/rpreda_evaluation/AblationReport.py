#!/usr/bin/env python3
"""Audit and summarize retained four-mode ablations without rerunning samples.

Original runner summaries are retained verbatim. Outcome variation is reported
separately from process failure; no timeout or inconvenient result is dropped.
"""
from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import pathlib
import shutil
import statistics
from typing import Any

from CoverageScalabilityRunner import (
    REQUIRED_SCALABILITY_BUILD_FEATURES,
    _distribution,
    _formula_ir_projection,
    _normalized_manifest,
    _validate_synthetic,
)

MODES = ("site_scan", "cfg_icfg", "formula_smt", "full")


def read(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def sha(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1048576), b""):
            digest.update(block)
    return digest.hexdigest()


def canonical(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def write(path: pathlib.Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")


def csv_file(path: pathlib.Path, rows: list[dict]) -> None:
    if not rows:
        return
    keys = list(dict.fromkeys(k for row in rows for k in row))
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, sort_keys=True) if isinstance(v, (dict, list)) else v for k, v in row.items()})


def proof_structure(manifest: dict) -> dict:
    """Keep proof inputs/IR; certificates and solver outcomes are outputs."""
    result = _normalized_manifest(manifest)
    result["refinement"] = _formula_ir_projection(manifest.get("refinement", {}))
    result.pop("parallel_certificate", None)
    return result


def expected_phases(mode: str) -> dict[str, bool]:
    cfg = mode != "site_scan"
    smt = mode in ("formula_smt", "full")
    return {
        "cfg_construction": cfg, "call_graph_construction": cfg,
        "effect_analysis": cfg, "icfg_construction": cfg,
        "summary_analysis": cfg, "refinement_generation": smt,
        "refinement_solver": smt, "refinement_total": smt,
        "certificate_generation": mode == "full", "analysis_total": cfg,
        "manifest_emission": True,
    }


def variation_details(paths: list[pathlib.Path]) -> dict:
    structures, outcomes, statuses = [], [], []
    for path in paths:
        value = read(path)
        structures.append(canonical(proof_structure(value)))
        outcomes.append(canonical(_normalized_manifest(value)))
        statuses.append(dict(collections.Counter(
            item.get("solver_result", {}).get("status", "NotRun")
            for item in value.get("refinement", {}).get("proof_obligations", [])
        )))
    return {"proof_input_structure_stable": len(set(structures)) == 1,
            "normalized_output_stable": len(set(outcomes)) == 1,
            "proof_input_sha256": structures,
            "normalized_output_sha256": outcomes, "solver_status_samples": statuses}


def audit(root: pathlib.Path, output: pathlib.Path) -> dict:
    output.mkdir(parents=True, exist_ok=True)
    errors, warnings, cases, rows, samples, changes = [], [], [], [], [], []
    source_hashes, case_sets, versions = {}, {}, {}
    total_raw = 0
    for mode in MODES:
        path = root / mode / "scalability" / "scalability.json"
        payload = read(path)
        meta = payload["metadata"]
        n, warmups = int(meta["measured_repetitions"]), int(meta["warmup_repetitions"])
        if (n, warmups) != (5, 3):
            errors.append(f"{mode}: expected 3 warmups and 5 measurements, got {warmups}/{n}")
        if meta.get("analysis_mode") != mode:
            errors.append(f"{mode}: mismatching summary mode")
        versions[mode] = {k: meta.get(k) for k in (
            "config_sha256", "analysis_driver_sha256", "linked_transpiler_library_sha256")}
        original = output / "original_summaries" / mode
        original.mkdir(parents=True, exist_ok=True)
        for item in path.parent.iterdir():
            if item.is_file():
                shutil.copy2(item, original / item.name)
        by_case = collections.defaultdict(list)
        for item in payload["samples"]:
            by_case[item["case_id"]].append(item)
        mode_rows = []
        status_totals = collections.Counter()
        raw_count, drift_count = 0, 0
        case_sets[mode] = sorted(p["case_id"] for p in payload["points"])
        source_hashes[mode] = {}
        for point in payload["points"]:
            case_id = point["case_id"]
            base = root / mode / "work" / "ablation" / case_id
            source = sorted(base.glob("*.prd"))
            if len(source) != 1:
                errors.append(f"{mode}/{case_id}: expected one source"); continue
            source_hashes[mode][case_id] = sha(source[0])
            records = sorted(by_case[case_id], key=lambda v: v["repeat"])
            if len(records) != n:
                errors.append(f"{mode}/{case_id}: only {len(records)}/{n} measured records")
            paths = []
            case_samples = []
            directories = [(f"warmup_{i:02}", False) for i in range(warmups)] + [(f"sample_{i:02}", True) for i in range(n)]
            for directory, measured in directories:
                sample_dir = base / directory
                required = [sample_dir / f for f in ("process_metrics.json", "analysis_metrics.json", "relay_protocol.json")]
                if not all(p.is_file() for p in required):
                    errors.append(f"{mode}/{case_id}/{directory}: missing raw file"); continue
                process, profile = read(required[0]), read(required[1])
                raw_count += 1
                if process.get("returncode") != 0 or process.get("timed_out") or process.get("launch_error") or profile.get("status") != "Compiled":
                    errors.append(f"{mode}/{case_id}/{directory}: process/compile failure")
                if profile.get("analysis_mode") != mode or profile.get("build_features") != REQUIRED_SCALABILITY_BUILD_FEATURES:
                    errors.append(f"{mode}/{case_id}/{directory}: mode or build feature mismatch")
                actual_phases = profile.get("analysis_phases", {})
                for phase, active in expected_phases(mode).items():
                    if phase not in actual_phases or (actual_phases[phase]["invocations"] > 0) != active:
                        errors.append(f"{mode}/{case_id}/{directory}: incorrect phase {phase}")
                if measured:
                    paths.append(required[2])
                    record = next((r for r in records if r["repeat"] == int(directory[-2:])), None)
                    if record is None:
                        continue
                    status_totals.update(record["solver_status"])
                    sample = {"analysis_mode": mode, "case_id": case_id,
                              "repeat": record["repeat"], "sweep_axis": point["sweep_axis"],
                              "source_sha256": source_hashes[mode][case_id],
                              "process_wall_ms": process["wall_time_ms"],
                              "pipeline_ms": profile["driver_timings"]["pipeline_time_ns"] / 1e6,
                              "downstream_analysis_ms": profile["total_analysis_ms"],
                              "peak_rss_kib": process["peak_rss_kib"],
                              "solver_goals": record["solver_goal_count"],
                              "solver_status": record["solver_status"],
                              "raw_directory": f"raw/{mode}/work/ablation/{case_id}/{directory}"}
                    case_samples.append(sample)
                    samples.append(sample)
            if len(case_samples) != n or not paths:
                continue
            representative = read(paths[0])
            validation = _validate_synthetic(representative, read(base / "generator_metadata.json"), mode)
            if not validation["valid"]:
                errors.append(f"{mode}/{case_id}: source structure validation failed: {validation['mismatches']}")
            diagnostic = None
            if point["status"] != "Completed":
                diagnostic = variation_details(paths)
                diagnostic.update({"analysis_mode": mode, "case_id": case_id, "original_status": point["status"], "original_reason": point.get("reason"), "original_drift": point.get("drift")})
                changes.append(diagnostic)
                if diagnostic["proof_input_structure_stable"] and not diagnostic["normalized_output_stable"]:
                    drift_count += 1
                    warnings.append(f"{mode}/{case_id}: solver/certificate output varies; all original outcomes retained")
                else:
                    errors.append(f"{mode}/{case_id}: {point['status']}")
            cert = representative.get("parallel_certificate", {}).get("functions", [])
            case = {"analysis_mode": mode, "case_id": case_id, "sweep_axis": point["sweep_axis"],
                    "source_sha256": source_hashes[mode][case_id], "measured_repetitions": n,
                    "original_runner_status": point["status"],
                    "report_status": "MeasuredWithOutputVariation" if diagnostic and diagnostic["proof_input_structure_stable"] else point["status"],
                    "raw_validation": validation, "source_dimensions": point.get("derived", read(base / "generator_metadata.json")["derived"]),
                    "formula_constraints": records[0]["formula_ir_constraint_count"],
                    "solver_goals_per_sample": records[0]["solver_goal_count"],
                    "certificate_functions_first_sample": len(cert),
                    "pair_relations_first_sample": sum(len(f.get("pair_relations", [])) for f in cert),
                    "manifest_bytes_median": statistics.median(r["manifest_size_bytes"] for r in records),
                    "solver_status_samples": [r["solver_status"] for r in records]}
            for key in ("process_wall_ms", "pipeline_ms", "downstream_analysis_ms", "peak_rss_kib"):
                case[key] = _distribution([s[key] for s in case_samples])
                case[key + "_median"] = case[key]["median"]
                case[key + "_iqr"] = case[key]["iqr"]
            cases.append(case)
            mode_rows.append(case)
        stability = meta.get("timing_stability", {})
        if mode != "site_scan" and stability.get("status") == "ExceedsAdvisoryThreshold":
            warnings.append(f"{mode}: timing stability exceeds the 1.2 advisory ratio; no causal speedup claim")
        row = {"analysis_mode": mode, "configured_cases": len(payload["points"]),
               "fully_measured_cases": len(mode_rows), "original_stable_cases": sum(p["status"] == "Completed" for p in payload["points"]),
               "output_variable_cases": drift_count, "raw_process_runs": raw_count,
               "measured_process_runs": sum(c["measured_repetitions"] for c in mode_rows),
               "warmups_per_case": warmups, "measurements_per_case": n,
               "z3_enabled": True, "solver_goals_one_pass": sum(c["solver_goals_per_sample"] for c in mode_rows),
               "solver_status_all_measured": dict(status_totals),
               "solver_timeouts_all_measured": status_totals.get("Timeout", 0),
               "certificate_functions_one_pass": sum(c["certificate_functions_first_sample"] for c in mode_rows),
               "pair_relations_one_pass": sum(c["pair_relations_first_sample"] for c in mode_rows),
               "round_timing_drift_ratio": stability.get("round_median_max_over_min") if mode != "site_scan" else None}
        for key in ("process_wall_ms", "pipeline_ms", "downstream_analysis_ms", "peak_rss_kib"):
            row[key + "_median_of_case_medians"] = statistics.median(c[key]["median"] for c in mode_rows) if mode_rows else None
        rows.append(row)
        total_raw += raw_count
    if any(case_sets[m] != case_sets[MODES[0]] for m in MODES):
        errors.append("modes use different case sets")
    if any(source_hashes[m] != source_hashes[MODES[0]] for m in MODES):
        errors.append("modes use different source bytes")
    if any(versions[m] != versions[MODES[0]] for m in MODES):
        errors.append("config or compiler binary differs between modes")
    result = {"schema_version": 1, "raw_run_root": str(root), "rows": rows,
              "cases": cases, "output_variations": changes, "errors": errors,
              "warnings": warnings, "compiler_hashes": versions,
              "raw_process_runs": total_raw, "measured_process_runs": len(samples),
              "unique_source_programs": len(set(source_hashes[MODES[0]].values())),
              "scope": "PREDA-native synthetic capability/cost ablation; no Native execution or runtime scheduling experiment",
              "timing_aggregation": "median of 40 per-configuration medians; each configuration has five retained measurements",
              "stage_timer_note": "site_scan analysis_total is an unentered downstream phase, not zero scan/compile cost; use process_wall_ms/pipeline_ms",
              "solver_goal_note": "counts solver-facing proof obligations, not internal Z3 check() calls; a goal can include assumption-consistency and negated-goal checks",
              "mode_order_note": "modes ran sequentially; cases were shuffled within each round; host drift limits cross-mode timing inference",
              "binding_note": "standalone manifests are unbound; bound_manifest build flag does not establish runtime artifact binding"}
    write(output / "ablation_summary.json", result)
    csv_file(output / "ablation_summary.csv", rows)
    csv_file(output / "ablation_cases.csv", cases)
    csv_file(output / "ablation_samples.csv", samples)
    lines = ["# R-PREDA 分层消融实验结果", "", "## 执行结果", "",
             f"四种模式共保留 {total_raw} 次进程运行，含 {len(samples)} 次正式测量。每个配置预热 3 次、测量 5 次。",
             f"40 个配置对应 {result['unique_source_programs']} 份不同的生成源程序；重复基准配置不视为独立应用。", "",
             "| 模式 | 完整测量配置 | 输出稳定配置 | Z3 goals/配置集一遍 | 进程耗时 ms | 峰值 RSS MiB |",
             "|---|---:|---:|---:|---:|---:|"]
    for row in rows:
        lines.append("| {analysis_mode} | {fully_measured_cases}/{configured_cases} | {original_stable_cases}/{configured_cases} | {solver_goals_one_pass} | {wall:.3f} | {rss:.2f} |".format(
            **row, wall=row["process_wall_ms_median_of_case_medians"] or 0,
            rss=(row["peak_rss_kib_median_of_case_medians"] or 0) / 1024))
    lines += ["", "耗时与内存列是各配置五次测量中位数的再中位数。逐配置四分位数、IQR 和每次原始值分别保存在 cases 与 samples 文件。", "",
              "## 数据检查", "",
              "- 核对各模式的配置集合、输入源文件字节、驱动程序和转译器哈希。",
              "- 核对每次运行的退出状态、所选模式、Z3 构建特征及各分析阶段调用次数。",
              "- 原始评测器的摘要与诊断保存在 original_summaries，未修改原始超时或求解结果。",
              "- site_scan 的同步调用深度在修订后的报告中为 null；该模式未建立同步调用图。",
              "- site_scan 的 analysis_total=0 仅说明后续分析阶段没有进入。扫描、解析、编译和序列化仍有开销。", "",
              "## 解释范围", "",
              "这些实验检查逐层分析能力和成本。Formula/SMT 层的 Proved、Disproved、Unsupported 和 Unknown 均需分别报告。Disproved 表示某项待证性质存在模型反例，不能直接计作发现合约漏洞。",
              "表中的 SMT goals 统计进入求解后端的证明义务，不等于 Z3 内部 check() 调用次数；一个义务可包含假设一致性和否定目标两个检查。EstablishedByConstruction 和 Unsupported 不计作成功求解。",
              "完整模式进一步输出 pairwise 与资源边界证书。本实验没有执行 Native 合约，不提供调度优化、吞吐提升或最终状态等价证据。",
              "模式按顺序运行，并未跨模式交错；计时波动限制了跨模式性能归因。五个重复样本的 p95 仅为描述性插值。", "",
              "## 原始诊断与限制", ""]
    lines.extend("- " + value for value in warnings)
    if not warnings:
        lines.append("未发现输出波动或计时稳定性警告。")
    if errors:
        lines += ["", "## 未通过的检查", ""] + ["- " + value for value in errors]
    lines += ["", "## 文件", "", "- ablation_summary.json / .csv：汇总与审计结论。",
              "- ablation_cases.csv：160 个模式—配置组合的分布。",
              "- ablation_samples.csv：全部正式样本。",
              "- original_summaries/：原始摘要、图表和诊断。",
              "- raw_runs.tar.gz（交付打包时生成）：全部预热、正式样本和生成合约。", ""]
    (output / "REPORT.md").write_text("\n".join(lines), encoding="utf-8")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", type=pathlib.Path, required=True)
    parser.add_argument("--output-root", type=pathlib.Path, required=True)
    args = parser.parse_args()
    result = audit(args.run_root.resolve(), args.output_root.resolve())
    print(json.dumps({k: result[k] for k in ("raw_process_runs", "measured_process_runs", "unique_source_programs", "errors", "warnings")}, ensure_ascii=False))
    return 1 if result["errors"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
