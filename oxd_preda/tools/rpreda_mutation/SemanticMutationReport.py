#!/usr/bin/env python3
"""Detection-layer breakdown for the R-PREDA semantic mutation study."""

from __future__ import annotations

import csv
import json
import pathlib
from collections import Counter
from typing import Any, Dict, Mapping, MutableMapping, Sequence


LAYERS = ("static", "z3", "certificate", "runtime")


def _rate(numerator: int, denominator: int) -> float | None:
    return None if denominator == 0 else numerator / denominator


def _layers(record: Mapping[str, Any]) -> set[str]:
    raw = record.get("detection_layers", [])
    if not isinstance(raw, Sequence) or isinstance(raw, (str, bytes)):
        return set()
    return {str(value) for value in raw if str(value) in LAYERS}


def _block(records: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    total = len(records)
    per_layer: Dict[str, Any] = {}
    for layer in LAYERS:
        eligible = sum(
            layer not in set(record.get("ineligible_layers", []))
            for record in records
        )
        detected = sum(layer in _layers(record) for record in records)
        unique = sum(_layers(record) == {layer} for record in records)
        per_layer[layer] = {
            "detected_mutants": detected,
            "eligible_mutants": eligible,
            "detection_rate": _rate(detected, eligible),
            "unique_detections": unique,
        }
    combinations = Counter(
        "+".join(layer for layer in LAYERS if layer in _layers(record)) or "none"
        for record in records
    )
    multi_layer = sum(len(_layers(record)) > 1 for record in records)
    deep_layers = {"z3", "certificate", "runtime"}
    any_deep = sum(
        bool(_layers(record).intersection(deep_layers))
        for record in records
    )
    detected_without_static = sum(
        "static" not in _layers(record)
        and bool(_layers(record).intersection(deep_layers))
        for record in records
    )
    return {
        "total_mutants": total,
        "per_layer": per_layer,
        "layer_combinations": dict(sorted(combinations.items())),
        "multi_layer_mutants": multi_layer,
        "multi_layer_rate": _rate(multi_layer, total),
        # Keep "beyond static" for the actual incremental contribution: a
        # deeper layer detects the mutant while the static layer does not.
        "detected_beyond_static_mutants": detected_without_static,
        "detected_beyond_static_rate": _rate(detected_without_static, total),
        # Separately expose overlap. This is evidence supplied by a deeper
        # layer, not an incremental detection over static analysis.
        "mutants_with_deep_layer_evidence": any_deep,
        "mutants_with_deep_layer_evidence_rate": _rate(any_deep, total),
    }


def calculate_breakdown(records: Sequence[Mapping[str, Any]]) -> Mapping[str, Any]:
    by_category: MutableMapping[str, Any] = {}
    for category in sorted(
        {str(record.get("semantic_category", "Uncategorized")) for record in records}
    ):
        by_category[category] = _block(
            [record for record in records if str(record.get("semantic_category", "Uncategorized")) == category]
        )
    by_benchmark: MutableMapping[str, Any] = {}
    for benchmark in sorted(
        {str(record.get("benchmark", "synthetic")) for record in records}
    ):
        by_benchmark[benchmark] = _block(
            [record for record in records if str(record.get("benchmark", "synthetic")) == benchmark]
        )
    matrix = []
    for record in records:
        layers = _layers(record)
        matrix.append(
            {
                "benchmark": record.get("benchmark", "synthetic"),
                "mutation_id": record.get("mutation_id", ""),
                "mutation_type": record.get("mutation_type", ""),
                "semantic_category": record.get("semantic_category", "Uncategorized"),
                "static": "static" in layers,
                "z3": "z3" in layers,
                "certificate": "certificate" in layers,
                "runtime": "runtime" in layers,
                "formula_ir_changed": record.get("analysis", {})
                .get("refinement", {})
                .get("formula_ir_changed"),
                "final_classification": record.get("final_classification", ""),
            }
        )
    return {
        "schema_version": 1,
        "layers": list(LAYERS),
        "overall": _block(records),
        "by_category": by_category,
        "by_benchmark": by_benchmark,
        "matrix": matrix,
    }


def write_detection_breakdown(
    output_directory: pathlib.Path, records: Sequence[Mapping[str, Any]]
) -> Mapping[str, Any]:
    output_directory.mkdir(parents=True, exist_ok=True)
    payload = calculate_breakdown(records)
    json_path = output_directory / "detection_breakdown.json"
    temporary_json = json_path.with_name(json_path.name + ".tmp")
    temporary_json.write_text(
        json.dumps(payload, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary_json.replace(json_path)

    csv_path = output_directory / "detection_breakdown.csv"
    temporary_csv = csv_path.with_name(csv_path.name + ".tmp")
    fields = (
        "benchmark",
        "mutation_id",
        "mutation_type",
        "semantic_category",
        "static",
        "z3",
        "certificate",
        "runtime",
        "formula_ir_changed",
        "final_classification",
    )
    with temporary_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(payload["matrix"])
    temporary_csv.replace(csv_path)
    return payload
