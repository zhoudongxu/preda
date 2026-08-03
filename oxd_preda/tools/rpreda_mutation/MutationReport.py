#!/usr/bin/env python3
"""Deterministic JSON/CSV reporting for the R-PREDA mutation study."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Sequence


CLASSIFICATIONS: Sequence[str] = (
    "CompilerRejected",
    "StaticProtocolMismatch",
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
    "Unsupported",
    "Survived",
    "InfrastructureFailure",
)

DETECTED = {
    "CompilerRejected",
    "StaticProtocolMismatch",
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
}

# Keep a stable presentation order.  ``DETECTED`` remains a set for the
# existing final-classification metrics, while these tuples describe evidence
# recorded in ``detected_by`` at individual pipeline stages.
DETECTION_STAGES: Sequence[str] = (
    "CompilerRejected",
    "StaticProtocolMismatch",
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
)

DEEPER_DETECTION_STAGES: Sequence[str] = (
    "Z3Disproved",
    "CertificateViolation",
    "RuntimeStrictMismatch",
)

CSV_FIELDS: Sequence[str] = (
    "mutation_id",
    "mutation_type",
    "benchmark",
    "semantic_category",
    "contract",
    "location",
    "detection_method",
    "final_classification",
    "detected_by",
    "detection_layers",
    "description",
    "compile_status",
    "static_protocol_changed",
    "z3_status",
    "certificate_status",
    "runtime_status",
    "reason",
)


class ReportError(RuntimeError):
    pass


def _rate(numerator: int, denominator: int) -> float | None:
    return None if denominator == 0 else numerator / denominator


def _classification_counts(records: Iterable[Mapping[str, Any]]) -> Dict[str, int]:
    counts = {classification: 0 for classification in CLASSIFICATIONS}
    for record in records:
        classification = str(record.get("final_classification", ""))
        if classification not in counts:
            raise ReportError(f"unknown final classification {classification!r}")
        counts[classification] += 1
    return counts


def _record_evidence(record: Mapping[str, Any]) -> set[str]:
    """Return normalized per-stage evidence, including older final-only rows."""

    raw = record.get("detected_by", [])
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes)):
        raw = []
    evidence = {str(value) for value in raw if str(value)}
    final = str(record.get("final_classification", ""))
    if final in DETECTED or final == "InfrastructureFailure":
        evidence.add(final)
    return evidence


def _analysis_section(record: Mapping[str, Any], name: str) -> Mapping[str, Any]:
    analysis = record.get("analysis", {})
    if not isinstance(analysis, Mapping):
        return {}
    section = analysis.get(name, {})
    return section if isinstance(section, Mapping) else {}


def _z3_unsupported_count(record: Mapping[str, Any]) -> int:
    z3 = _analysis_section(record, "z3")
    unsupported = z3.get("unsupported", [])
    if isinstance(unsupported, Sequence) and not isinstance(
        unsupported, (str, bytes)
    ):
        count = len(unsupported)
    else:
        count = 0
    statuses = z3.get("statuses", {})
    if isinstance(statuses, Mapping):
        try:
            count = max(count, int(statuses.get("Unsupported", 0)))
        except (TypeError, ValueError):
            pass
    if count == 0 and record.get("z3_status") == "Unsupported":
        count = 1
    return count


def _runtime_skipped_unsupported_count(record: Mapping[str, Any]) -> int:
    runtime = _analysis_section(record, "runtime")
    try:
        return max(0, int(runtime.get("skipped_unsupported", 0)))
    except (TypeError, ValueError):
        return 0


def _operator_inapplicable(record: Mapping[str, Any]) -> bool:
    return str(record.get("generation_status", "")) == "Unsupported"


def _any_stage_infrastructure_failure(record: Mapping[str, Any]) -> bool:
    if "InfrastructureFailure" in _record_evidence(record):
        return True
    if str(record.get("compile_status", "")) == "InfrastructureFailure":
        return True
    if str(record.get("runtime_status", "")) == "InfrastructureFailure":
        return True
    z3 = _analysis_section(record, "z3")
    failures = z3.get("infrastructure_failures", [])
    return bool(failures)


def _any_stage_unsupported(record: Mapping[str, Any]) -> bool:
    return (
        _operator_inapplicable(record)
        or _z3_unsupported_count(record) > 0
        or _runtime_skipped_unsupported_count(record) > 0
        or str(record.get("final_classification", "")) == "Unsupported"
    )


def _fully_analyzable(record: Mapping[str, Any]) -> bool:
    return not _any_stage_infrastructure_failure(record) and not _any_stage_unsupported(
        record
    )


def _stage_metrics(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    total = len(records)
    evidence_by_record = [_record_evidence(record) for record in records]
    detection_evidence = [
        evidence.intersection(DETECTION_STAGES) for evidence in evidence_by_record
    ]
    result: Dict[str, Any] = {}
    for stage in DETECTION_STAGES:
        detected = sum(stage in evidence for evidence in detection_evidence)
        unique = sum(evidence == {stage} for evidence in detection_evidence)
        result[stage] = {
            "detected_mutants": detected,
            "rate_denominator_mutants": total,
            "detection_rate": _rate(detected, total),
            "unique_kills": unique,
            "unique_kill_rate": _rate(unique, total),
        }
    return result


def _deeper_stage_metrics(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    total = len(records)
    completed = [
        record
        for record in records
        if not _any_stage_infrastructure_failure(record)
    ]

    def detected(record: Mapping[str, Any]) -> bool:
        return bool(
            _record_evidence(record).intersection(DEEPER_DETECTION_STAGES)
        )

    detected_all = sum(detected(record) for record in records)
    detected_completed = sum(detected(record) for record in completed)
    fully_analyzable = [record for record in records if _fully_analyzable(record)]
    return {
        "excluded_stages": ["CompilerRejected", "StaticProtocolMismatch"],
        "included_stages": list(DEEPER_DETECTION_STAGES),
        "rate_denominator_mutants": total,
        "detected_mutants": detected_all,
        "detection_rate": _rate(detected_all, total),
        "infrastructure_free_mutants": len(completed),
        "infrastructure_free_detected_mutants": detected_completed,
        "infrastructure_free_detection_rate": _rate(
            detected_completed, len(completed)
        ),
        "fully_analyzable_mutants": len(fully_analyzable),
        "fully_analyzable_detected_mutants": sum(
            detected(record) for record in fully_analyzable
        ),
        "fully_analyzable_detection_rate": _rate(
            sum(detected(record) for record in fully_analyzable),
            len(fully_analyzable),
        ),
    }


def _unsupported_metrics(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    total = len(records)
    final = sum(
        str(record.get("final_classification", "")) == "Unsupported"
        for record in records
    )
    z3_counts = [_z3_unsupported_count(record) for record in records]
    runtime_counts = [
        _runtime_skipped_unsupported_count(record) for record in records
    ]
    inapplicable = sum(_operator_inapplicable(record) for record in records)
    z3_mutants = sum(count > 0 for count in z3_counts)
    runtime_mutants = sum(count > 0 for count in runtime_counts)
    return {
        "rate_denominator_mutants": total,
        "final_classification": {
            "mutants": final,
            "rate": _rate(final, total),
        },
        "z3": {
            "mutants_with_unsupported": z3_mutants,
            "mutant_rate": _rate(z3_mutants, total),
            "unsupported_obligations": sum(z3_counts),
        },
        "runtime": {
            "mutants_with_skipped_unsupported": runtime_mutants,
            "mutant_rate": _rate(runtime_mutants, total),
            "skipped_checks": sum(runtime_counts),
        },
        "operator_inapplicable": {
            "mutants": inapplicable,
            "rate": _rate(inapplicable, total),
        },
    }


def _metric_block(records: Sequence[Mapping[str, Any]]) -> Dict[str, Any]:
    counts = _classification_counts(records)
    total = len(records)
    infrastructure = counts["InfrastructureFailure"]
    evaluable = total - infrastructure
    detected = sum(counts[name] for name in DETECTED)
    unsupported = counts["Unsupported"]
    survived = counts["Survived"]
    supported = detected + survived
    stage_infrastructure_failures = sum(
        _any_stage_infrastructure_failure(record) for record in records
    )
    infrastructure_free = total - stage_infrastructure_failures
    fully_analyzable = sum(_fully_analyzable(record) for record in records)
    per_stage = _stage_metrics(records)
    unique_kill_counts = {
        stage: per_stage[stage]["unique_kills"] for stage in DETECTION_STAGES
    }
    return {
        "total_mutants": total,
        "evaluable_mutants": evaluable,
        "supported_mutants": supported,
        "detected_mutants": detected,
        "classification_counts": counts,
        "overall_detection_rate": _rate(detected, evaluable),
        "supported_detection_rate": _rate(detected, supported),
        "unsupported_rate": _rate(unsupported, evaluable),
        "survival_rate": _rate(survived, evaluable),
        "infrastructure_failure_rate": _rate(infrastructure, total),
        "per_stage_detection": per_stage,
        "unique_kill_counts": unique_kill_counts,
        "deeper_stage_score_excluding_static": _deeper_stage_metrics(records),
        "unsupported_breakdown": _unsupported_metrics(records),
        "any_stage_infrastructure_failure_mutants": stage_infrastructure_failures,
        "any_stage_infrastructure_failure_rate": _rate(
            stage_infrastructure_failures, total
        ),
        "full_pipeline_completed_mutants": infrastructure_free,
        "stage_infrastructure_failures": stage_infrastructure_failures,
        "full_pipeline_completion_rate": _rate(
            infrastructure_free, total
        ),
        "infrastructure_free_pipeline_completed_mutants": infrastructure_free,
        "infrastructure_free_pipeline_completion_rate": _rate(
            infrastructure_free, total
        ),
        "fully_analyzable_mutants": fully_analyzable,
        "fully_analyzable_rate": _rate(fully_analyzable, total),
        "fully_supported_pipeline_completed_mutants": fully_analyzable,
        "fully_supported_pipeline_completion_rate": _rate(
            fully_analyzable, total
        ),
    }


def calculate_metrics(
    records: Sequence[Mapping[str, Any]],
    controls: Sequence[Mapping[str, Any]],
) -> Dict[str, Any]:
    """Compute denominators explicitly; infrastructure failures never vanish."""

    overall = _metric_block(records)
    by_type: Dict[str, Any] = {}
    for mutation_type in sorted(
        {str(record.get("mutation_type", "")) for record in records}
    ):
        typed = [
            record
            for record in records
            if str(record.get("mutation_type", "")) == mutation_type
        ]
        by_type[mutation_type] = _metric_block(typed)

    usable_controls = [
        control
        for control in controls
        if control.get("final_classification")
        not in {"InfrastructureFailure", "Unsupported"}
    ]
    flagged_controls = [
        control
        for control in usable_controls
        if control.get("final_classification") in DETECTED
    ]
    infrastructure_controls = sum(
        control.get("final_classification") == "InfrastructureFailure"
        for control in controls
    )
    unsupported_controls = sum(
        control.get("final_classification") == "Unsupported"
        for control in controls
    )
    flagged_all_controls = sum(
        control.get("final_classification") in DETECTED for control in controls
    )
    return {
        **overall,
        "per_mutation_type": by_type,
        "negative_control_count": len(controls),
        "usable_negative_control_count": len(usable_controls),
        "flagged_negative_control_count": len(flagged_controls),
        "false_positive_rate": _rate(
            len(flagged_controls), len(usable_controls)
        ),
        "original_control_flag_rate": _rate(
            flagged_all_controls, len(controls)
        ),
        "negative_control_infrastructure_failures": infrastructure_controls,
        "negative_control_unsupported": unsupported_controls,
    }


def _atomic_text(path: pathlib.Path, contents: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(contents, encoding="utf-8")
    temporary.replace(path)


def write_json(path: pathlib.Path, value: Mapping[str, Any]) -> None:
    _atomic_text(
        path,
        json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
    )


def _location_text(location: Any) -> str:
    if not isinstance(location, Mapping):
        return ""
    line = location.get("line", 0)
    column = location.get("column", 0)
    end_line = location.get("end_line", 0)
    end_column = location.get("end_column", 0)
    return f"{line}:{column}-{end_line}:{end_column}"


def write_csv(path: pathlib.Path, records: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(CSV_FIELDS))
        writer.writeheader()
        for record in records:
            row: MutableMapping[str, Any] = {
                field: record.get(field, "") for field in CSV_FIELDS
            }
            row["location"] = _location_text(record.get("location"))
            row["detected_by"] = ";".join(record.get("detected_by", []))
            row["detection_layers"] = ";".join(
                record.get("detection_layers", [])
            )
            writer.writerow(row)
    temporary.replace(path)


def write_results(
    output_directory: pathlib.Path,
    metadata: Mapping[str, Any],
    records: Sequence[Mapping[str, Any]],
    controls: Sequence[Mapping[str, Any]],
) -> Mapping[str, Any]:
    metrics = calculate_metrics(records, controls)
    payload: Dict[str, Any] = {
        "schema_version": 1,
        "metadata": dict(metadata),
        "metrics": metrics,
        "negative_controls": list(controls),
        "mutations": list(records),
    }
    write_json(output_directory / "mutation.json", payload)
    write_csv(output_directory / "mutation.csv", records)
    return payload


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=pathlib.Path)
    args = parser.parse_args(argv)
    payload = json.loads(args.report.read_text(encoding="utf-8"))
    metrics = payload.get("metrics")
    if not isinstance(metrics, Mapping):
        raise ReportError("report has no metrics object")
    print(json.dumps(metrics, indent=2, sort_keys=True, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
