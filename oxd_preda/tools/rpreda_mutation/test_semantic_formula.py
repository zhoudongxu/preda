#!/usr/bin/env python3

import csv
import json
import os
import pathlib
import shutil
import tempfile
import unittest

from SemanticFormulaComparator import solve_preservation
from SemanticMutationReport import calculate_breakdown, write_detection_breakdown


def sort(width=32):
    return {"kind": "UnsignedBitVector", "bit_width": width}


def symbol(name, width=32):
    return {
        "kind": "Symbol",
        "sort": sort(width),
        "symbol_id": name,
        "children": [],
    }


def literal(value, width=32):
    return {
        "kind": "BitVectorLiteral",
        "sort": sort(width),
        "literal_value": f"{value}u{width}",
        "children": [],
    }


def add(left, right):
    return {
        "kind": "Binary",
        "sort": left["sort"],
        "operator": "+",
        "children": [left, right],
    }


TRUE = {
    "kind": "BoolLiteral",
    "sort": {"kind": "Bool"},
    "literal_value": "true",
    "children": [],
}


class FormulaDifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("RPREDA_Z3_BIN", "")
        cls.z3 = pathlib.Path(configured or shutil.which("z3") or "")

    def require_z3(self):
        if not self.z3.is_file():
            self.skipTest("external Z3 CLI is not available")

    def test_non_circular_plus_one_is_disproved(self):
        self.require_z3()
        x = symbol("parameter.x")
        result = solve_preservation(TRUE, x, add(x, literal(1)), self.z3)
        self.assertEqual(result["status"], "Disproved")
        self.assertIn("parameter.x", result["symbol_map"].values())

    def test_equivalent_fixed_width_formula_is_proved(self):
        self.require_z3()
        x = symbol("parameter.x")
        wrapped = add(x, literal(0))
        result = solve_preservation(TRUE, x, wrapped, self.z3)
        self.assertEqual(result["status"], "Proved")

    def test_unknown_is_unsupported_not_fresh(self):
        self.require_z3()
        unknown = {
            "kind": "Unknown",
            "sort": {"kind": "Unknown"},
            "unknown_reason": "opaque ternary",
            "children": [],
        }
        result = solve_preservation(
            TRUE, symbol("parameter.x"), unknown, self.z3
        )
        self.assertEqual(result["status"], "Unsupported")
        self.assertIn("opaque ternary", result["reason"])

    def test_goal_cannot_be_reused_as_an_assumption(self):
        self.require_z3()
        a = {
            "kind": "Symbol",
            "sort": {"kind": "Bool"},
            "symbol_id": "a",
            "children": [],
        }
        b = {
            "kind": "Symbol",
            "sort": {"kind": "Bool"},
            "symbol_id": "b",
            "children": [],
        }
        circular = {
            "kind": "Binary",
            "sort": {"kind": "Bool"},
            "operator": "==",
            "children": [a, b],
        }
        result = solve_preservation(circular, a, b, self.z3)
        self.assertEqual(result["status"], "EncodingError")
        self.assertIn("circular", result["reason"])

    def test_nary_bitvector_addition_uses_bitvector_semantics(self):
        self.require_z3()
        x = symbol("parameter.x")
        nary = {
            "kind": "Nary",
            "sort": sort(),
            "operator": "+",
            "children": [x, literal(1), literal(4294967295)],
        }
        result = solve_preservation(TRUE, x, nary, self.z3)
        self.assertEqual(result["status"], "Proved")


class DetectionBreakdownTests(unittest.TestCase):
    def test_layers_are_multi_label_and_denominators_are_explicit(self):
        records = [
            {
                "benchmark": "Synthetic",
                "mutation_id": "target",
                "mutation_type": "TargetArithmeticPerturb",
                "semantic_category": "Target",
                "detection_layers": ["static", "z3"],
                "ineligible_layers": ["certificate", "runtime"],
                "final_classification": "StaticProtocolMismatch",
                "analysis": {"refinement": {"formula_ir_changed": True}},
            },
            {
                "benchmark": "Synthetic",
                "mutation_id": "fault",
                "mutation_type": "RuntimeRelayDuplicate",
                "semantic_category": "Work",
                "detection_layers": ["runtime"],
                "ineligible_layers": ["static", "z3", "certificate"],
                "final_classification": "RuntimeStrictMismatch",
                "analysis": {"refinement": {"formula_ir_changed": None}},
            },
        ]
        payload = calculate_breakdown(records)
        self.assertEqual(payload["overall"]["multi_layer_mutants"], 1)
        self.assertEqual(
            payload["overall"]["mutants_with_deep_layer_evidence"], 2
        )
        self.assertEqual(
            payload["overall"]["detected_beyond_static_mutants"], 1
        )
        self.assertEqual(
            payload["overall"]["per_layer"]["z3"]["eligible_mutants"], 1
        )
        self.assertEqual(
            payload["overall"]["per_layer"]["runtime"]["eligible_mutants"], 1
        )

        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            write_detection_breakdown(root, records)
            self.assertEqual(
                json.loads((root / "detection_breakdown.json").read_text())[
                    "overall"
                ]["total_mutants"],
                2,
            )
            with (root / "detection_breakdown.csv").open(newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["z3"], "True")


if __name__ == "__main__":
    unittest.main()
