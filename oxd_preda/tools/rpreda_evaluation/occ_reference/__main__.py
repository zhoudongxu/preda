from __future__ import annotations

import argparse
import json
from pathlib import Path

from .engine import run_reference
from .adapter import load_native_normalized


def main() -> int:
    parser = argparse.ArgumentParser(description="Replay normalized R-PREDA relay records with deterministic OCC")
    parser.add_argument(
        "--input", type=Path, help="normalized OCC JSON input"
    )
    parser.add_argument(
        "--native-normalized",
        type=Path,
        help="Native correctness.normalized.json to adapt before replay",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--policy", choices=("fifo", "priority"), default="priority")
    parser.add_argument(
        "--allow-target-key-effects",
        action="store_true",
        help=(
            "sensitivity mode: allow target-key concurrency even though Native "
            "output has no read/write sets; do not use as correctness evidence"
        ),
    )
    parser.add_argument("--limit", type=int, default=None)
    args = parser.parse_args()
    if bool(args.input) == bool(args.native_normalized):
        parser.error("provide exactly one of --input or --native-normalized")
    if args.native_normalized:
        payload = load_native_normalized(
            args.native_normalized,
            opaque=not args.allow_target_key_effects,
            limit=args.limit,
        )
    else:
        payload = json.loads(args.input.read_text(encoding="utf-8"))
        if args.limit is not None:
            payload = dict(payload)
            payload["transactions"] = payload.get("transactions", payload.get("records", []))[
                : args.limit
            ]
    result = run_reference(payload, workers=args.workers, policy=args.policy)
    result["input"] = {
        "kind": "native_normalized" if args.native_normalized else "occ_json",
        "path": str(args.native_normalized or args.input),
        "conservative_opaque": bool(args.native_normalized and not args.allow_target_key_effects),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
