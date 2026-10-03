"""Semantic regression tests, including wrong-answer injection and oracle isolation."""
from __future__ import annotations

import copy
import json
import os
import pathlib
import tempfile
import unittest

import corpus
import oracle
from adapters import Adapter, Formula, Unavailable, upper
from report import score


class OracleTests(unittest.TestCase):
    def answers(self,program,domains=None):
        entry={"oracle_model":corpus.model(program,domains)}
        qs=[dict(id=p,property=p,site_a="A",site_b="B") for p in ["may_alias","mutual_exclusion","must_precede"]]
        qs += [dict(id=p,property=p) for p in ["direct_work","tree_work","depth"]]
        return {r["query_id"]:r for r in oracle.evaluate(entry,qs)}

    def test_alias_does_not_imply_mutex(self):
        r=self.answers([corpus.emit("A",0),corpus.emit("B",0)])
        self.assertTrue(r["may_alias"]["truth"]);self.assertFalse(r["mutual_exclusion"]["truth"])

    def test_disjoint_targets_still_coemit(self):
        r=self.answers([corpus.emit("A",0),corpus.emit("B",1)])
        self.assertFalse(r["may_alias"]["truth"]);self.assertFalse(r["mutual_exclusion"]["truth"])

    def test_precedence_is_not_conditional_source_order(self):
        r=self.answers([corpus.conditional("p",[corpus.emit("A",0)]),corpus.emit("B",1)],{"p":[False,True]})
        self.assertFalse(r["must_precede"]["truth"])
        self.assertEqual(r["must_precede"]["witness"]["input"],{"p":False})

    def test_mutation_of_guard_state(self):
        r=self.answers([corpus.conditional("p",[corpus.emit("A",0)]),["set","p","not p"],corpus.conditional("p",[corpus.emit("B",0)])],{"p":[False,True]})
        self.assertTrue(r["mutual_exclusion"]["truth"]);self.assertEqual(r["direct_work"]["truth"],1)

    def test_repeated_occurrences_and_prefix_precedence(self):
        r=self.answers([["repeat",3,[corpus.emit("A",0)]],corpus.emit("B",1)])
        self.assertEqual(r["direct_work"]["truth"],4);self.assertTrue(r["must_precede"]["truth"])
        r=self.answers([corpus.emit("B",0),corpus.emit("A",1),corpus.emit("B",0)])
        self.assertFalse(r["must_precede"]["truth"])

    def test_tree_work_counts_children_depth_uses_maximum(self):
        r=self.answers([corpus.emit("A",0,[corpus.emit("child",1),corpus.emit("child2",2)]),corpus.emit("B",1)])
        self.assertEqual((r["direct_work"]["truth"],r["tree_work"]["truth"],r["depth"]["truth"]),(2,4,2))

    def test_vacuous_precedence_is_marked(self):
        r=self.answers([corpus.emit("A",0)])
        self.assertTrue(r["must_precede"]["truth"]);self.assertTrue(r["must_precede"]["vacuous"])

    def test_unknown_expression_fails_closed(self):
        with self.assertRaises(ValueError):oracle.expression("f(x)",{"x":1})


class ScoringTests(unittest.TestCase):
    def test_bad_boolean_certificate_is_not_masked(self):
        self.assertEqual(score(dict(status="decided",value=True),dict(truth_kind="boolean",truth=False)),"wrong")

    def test_overapprox_bound_safe_but_underestimate_wrong(self):
        truth=dict(truth_kind="resource_supremum",truth=3)
        self.assertEqual(score(dict(status="bound",value=4),truth),"correct")
        self.assertEqual(score(dict(status="bound",value=2),truth),"wrong")

    def test_finite_bound_on_unbounded_behavior_wrong(self):
        self.assertEqual(score(dict(status="bound",value=999999),dict(truth_kind="resource_supremum",truth=None)),"wrong")

    def test_unknown_and_unsupported_are_separate(self):
        truth=dict(truth_kind="boolean",truth=True)
        self.assertEqual(score(dict(status="unknown"),truth),"unknown")
        self.assertEqual(score(dict(status="unsupported"),truth),"unsupported")

    def test_unknown_is_not_an_unbounded_answer(self):
        self.assertEqual(score(dict(status="unknown"),dict(truth_kind="resource_supremum",truth=None)),"unknown")

    def test_boolean_not_accepted_as_integer_bound(self):
        self.assertEqual(score(dict(status="bound",value=True),dict(truth_kind="resource_supremum",truth=1)),"wrong")


class FormulaTests(unittest.TestCase):
    def setUp(self):self.f=Formula()

    def literal(self,value,bits=8):
        return dict(kind="BitVectorLiteral",sort=dict(kind="UnsignedBitVector",bit_width=bits),literal_value=str(value)+"u"+str(bits),children=[])

    def test_unsigned_comparison_uses_unsigned_order(self):
        e=dict(kind="Binary",sort=dict(kind="Bool"),operator=">",children=[self.literal(255),self.literal(0)])
        self.assertEqual(self.f.check(self.f.convert(e))[0],"sat")

    def test_widening_preserves_all_uint8_values(self):
        for value in range(256):
            e=dict(kind="Cast",sort=dict(kind="UnsignedBitVector",bit_width=32),children=[self.literal(value)])
            self.assertEqual(self.f.z.simplify(self.f.convert(e)).as_long(),value)

    def test_unknown_formula_is_not_true(self):
        with self.assertRaises(Unavailable):self.f.convert(dict(kind="Unknown",sort=dict(kind="Unknown"),children=[]))

    def test_unknown_bound_is_not_zero(self):
        with self.assertRaises(Unavailable):upper(dict(kind="unknown",reason="loop bound missing"))

    def test_branch_bound_uses_maximum(self):
        self.assertEqual(upper(dict(kind="ite",children=[dict(kind="constant",value=3),dict(kind="constant",value=2)])),3)


@unittest.skipUnless(os.environ.get("RPREDA_SHARED_QUERY_RESULTS"),"set RPREDA_SHARED_QUERY_RESULTS for retained-run integration checks")
class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.root=pathlib.Path(os.environ["RPREDA_SHARED_QUERY_RESULTS"])
        self.c=json.loads((self.root/"corpus.json").read_text())

    def adapter(self,pid,mode):
        return Adapter(json.loads((self.root/"runs"/pid/mode/"rep_00/manifest.json").read_text()),mode)

    def ask(self,pid,mode,prop,a=None,b=None):
        e=next(e for e in self.c["entries"] if e["id"]==pid)
        public={k:v for k,v in e.items() if k!="oracle_model"}
        q=next(q for q in self.c["queries"] if q["entry_id"]==pid and q["property"]==prop and (a is None or (q["site_a"],q["site_b"])==(a,b)))
        return self.adapter(e["program_id"],mode).answer(public,q)

    def test_formula_baseline_can_answer_without_native_certificate(self):
        a=self.adapter("controlled_guard_distinct","formula_smt")
        self.assertFalse(a.cert)
        r=self.ask("controlled_guard_distinct","formula_smt","may_alias","A","B")
        self.assertEqual((r["status"],r["value"]),("decided",False))

    def test_cfg_has_real_cross_function_precedence(self):
        r=self.ask("controlled_sync_helper","cfg_icfg","must_precede","A","B")
        self.assertEqual((r["status"],r["value"]),("decided",True))

    def test_repeated_helper_counts_occurrences_not_unique_sites(self):
        r=self.ask("controlled_sync_repeat","cfg_icfg","direct_work")
        self.assertEqual((r["status"],r["value"]),("bound",2))

    def test_unknown_work_does_not_hide_known_relay_depth(self):
        r=self.ask("controlled_unbounded_loop","cfg_icfg","depth")
        self.assertEqual((r["status"],r["value"]),("bound",1))

    def test_helper_failure_does_not_continue_caller(self):
        adapter=self.adapter("controlled_sync_helper","cfg_icfg")
        entry=next(e for e in self.c["entries"] if e["id"]=="controlled_sync_helper")
        root,sites=adapter.resolve({k:v for k,v in entry.items() if k!="oracle_model"})
        for events,_,_,_ in adapter.paths(root):
            if sites["B"] in events:self.assertIn(sites["A"],events[:events.index(sites["B"])])

    def test_cfg_does_not_treat_structural_path_as_data_witness(self):
        r=self.ask("controlled_complementary_if","cfg_icfg","mutual_exclusion","A","B")
        self.assertEqual(r["status"],"unknown")

    def test_smt_excludes_complementary_conditions(self):
        r=self.ask("controlled_complementary_if","formula_smt","mutual_exclusion","A","B")
        self.assertEqual((r["status"],r["value"]),("decided",True))

    def test_unknown_state_update_cannot_produce_false_witness(self):
        r=self.ask("controlled_boolean_update","formula_smt","mutual_exclusion","A","B")
        self.assertNotEqual((r["status"],r["value"]),("decided",False))

    def test_oracle_model_is_rejected_at_inference_boundary(self):
        a=self.adapter("controlled_guard_equal","full")
        e=next(e for e in self.c["entries"] if e["id"]=="controlled_guard_equal")
        with self.assertRaises(ValueError):a.resolve(e)

    def test_injected_wrong_native_answer_is_retained_for_scoring(self):
        pid="controlled_guard_distinct";a=self.adapter(pid,"full")
        for c in a.cert.values():
            for p in c["pair_relations"]:
                if p["relation"]=="CoEmissionIndependent":p["relation"]="ProvedMayAlias"
        e=next(e for e in self.c["entries"] if e["id"]==pid)
        public={k:v for k,v in e.items() if k!="oracle_model"}
        q=next(q for q in self.c["queries"] if q["entry_id"]==pid and q["property"]=="may_alias")
        r=a.answer(public,q)
        self.assertEqual(score(r,dict(truth_kind="boolean",truth=False)),"wrong")


if __name__=="__main__":unittest.main()
