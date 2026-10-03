"""Adapters from Native correctness output to OCC reference records.

The Native normalized correctness format is a semantic transaction multiset.
It intentionally removes implementation-only timing fields and does not carry
read/write sets.  This adapter therefore keeps the observed target as a
scope key and marks every record opaque by default.  The OCC reference then
serializes unknown effects instead of inventing parallel independence.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping, Optional


def _as_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _target_scope(row: Mapping[str, Any], index: int) -> str:
    target = row.get("Target") or row.get("target")
    if target is None or str(target) == "":
        # A missing target is itself an unknown shared scope.  Keeping one
        # stable key prevents the replay from treating missing targets as
        # independent transactions.
        return "<unknown-target>"
    return str(target)


def native_rows_to_payload(
    rows: Iterable[Mapping[str, Any]],
    *,
    workload: str = "",
    variant: str = "",
    opaque: bool = True,
    limit: Optional[int] = None,
) -> Dict[str, Any]:
    """Convert normalized Native semantic rows to OCC input JSON.

    ``opaque=True`` is the publication-safe setting.  It records the target
    scope as a conservative read/write key but prevents the OCC reference
    from running unknown effects concurrently.  ``opaque=False`` is exposed
    only as an explicit sensitivity mode for debugging; it must not be used
    as Native correctness evidence.
    """
    records: List[Dict[str, Any]] = []
    for index, row in enumerate(rows):
        if limit is not None and index >= limit:
            break
        scope = _target_scope(row, index)
        origin_shard = _as_int(row.get("OriginateShardIndex"), -1)
        destination_shard = _as_int(row.get("ShardIndex"), -1)
        # The normalized file has no OriginateHeight.  BuildNum and the
        # observed list position provide stable, auditable ordering fields.
        build_num = _as_int(row.get("BuildNum"), 0)
        origin_order = _as_int(row.get("OriginateShardOrder"), index)
        destination_order = _as_int(row.get("ShardOrder"), index)
        function = str(row.get("Function", ""))
        identifier = f"native-{index:08d}"
        records.append(
            {
                "id": identifier,
                "source_order": index,
                "scope": scope,
                "reads": [scope],
                "writes": [scope],
                "work": 1,
                "depth": max(0, build_num),
                "predecessors": [],
                "opaque": bool(opaque),
                "source_shard": origin_shard,
                "destination_shard": destination_shard,
                "origin_order": origin_order,
                "destination_order": destination_order,
                "function": function,
                "effect_source": "target_scope_only",
            }
        )
    return {
        "schema_version": 2,
        "source": {
            "kind": "native_correctness_normalized",
            "workload": workload,
            "variant": variant,
            "effect_recovery": "target_scope_only",
            "opaque_default": bool(opaque),
            "record_count": len(records),
        },
        "transactions": records,
    }


def load_native_normalized(
    path: Path, *, opaque: bool = True, limit: Optional[int] = None
) -> Dict[str, Any]:
    """Load a Native ``correctness.normalized.json`` file."""
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, Mapping):
        raise ValueError(f"Native normalized output must be an object: {path}")
    rows = payload.get("confirmed_semantic_transaction_multiset")
    if not isinstance(rows, list):
        raise ValueError(
            "Native normalized output lacks confirmed_semantic_transaction_multiset"
        )
    return native_rows_to_payload(
        rows,
        workload=str(payload.get("workload", "")),
        variant=str(payload.get("variant", "")),
        opaque=opaque,
        limit=limit,
    )

