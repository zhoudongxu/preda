"""Mode-local semantic answers. This module cannot access oracle models/labels."""
from __future__ import annotations

import re
import time


class Unavailable(Exception):
    pass


def answer(status, value=None, reason="", origin="", **evidence):
    return dict(status=status, value=value, reason=reason, origin=origin, **evidence)


def upper(expr):
    kind=expr.get("kind")
    if kind=="constant":return int(expr["value"])
    children=expr.get("children",[])
    if kind in ["ite","max"]:return max(upper(c) for c in children)
    if kind in ["add","sum"]:return sum(upper(c) for c in children)
    if kind in ["multiply","mul","product"]:
        values=[upper(c) for c in children];product=1
        for value in values:product*=value
        return product
    raise Unavailable("no constant uniform bound: "+str(expr.get("reason",kind)))


def literal(site):
    text=site.get("target",{}).get("text","").strip("() ")
    hit=re.fullmatch(r"(0x[0-9a-fA-F]+|[0-9]+)(?:u(?:8|16|32|64|128|256|512)?|i(?:b|8|16|32|64)?)?",text)
    return int(hit.group(1),16 if hit and hit.group(1).startswith("0x") else 10) if hit else None


class Formula:
    def __init__(self):
        import z3
        self.z=z3;self.symbols={};self.address=z3.DeclareSort("SharedQueryAddress")
        self.calls=[]

    def sort(self,s):
        z=self.z;k=s.get("kind")
        if k=="Bool":return z.BoolSort()
        if k=="Int":return z.IntSort()
        if k=="Address":return self.address
        if k in ["UnsignedBitVector","SignedBitVector"]:return z.BitVecSort(s["bit_width"])
        raise Unavailable("unsupported formula sort "+str(k))

    def convert(self,e):
        z=self.z;kind=e.get("kind");s=e.get("sort",{});op=e.get("operator")
        children=e.get("children",[])
        if kind=="Unknown":raise Unavailable(e.get("unknown_reason","unknown formula"))
        if kind=="Symbol":
            key=e["symbol_id"];sort=self.sort(s)
            if key in self.symbols and self.symbols[key].sort()!=sort:raise Unavailable("inconsistent symbol sorts")
            if key not in self.symbols:self.symbols[key]=z.Const(key,sort)
            return self.symbols[key]
        if kind in ["IntLiteral","BitVectorLiteral","BoolLiteral"]:
            text=str(e.get("literal_value",e.get("source_text",e.get("text",""))))
            if kind=="BoolLiteral":
                if text not in ["true","false","1","0"]:raise Unavailable("bad boolean literal")
                return z.BoolVal(text in ["true","1"])
            hit=re.fullmatch(r"(-?(?:0x[0-9a-fA-F]+|[0-9]+))(?:u(?:8|16|32|64|128|256|512)?|i(?:b|8|16|32|64|128|256|512)?)?",text)
            if not hit:raise Unavailable("bad integer literal "+text)
            value=int(hit.group(1),16 if "0x" in hit.group(1) else 10)
            return z.BitVecVal(value,s["bit_width"]) if kind=="BitVectorLiteral" else z.IntVal(value)
        args=[self.convert(c) for c in children]
        if kind=="Group":return args[0]
        if kind=="Cast":
            value=args[0];dest=self.sort(s)
            if value.sort()==dest:return value
            if z.is_bv(value) and z.is_bv_sort(dest):
                delta=dest.size()-value.size()
                if delta>=0:
                    return z.SignExt(delta,value) if children[0].get("sort",{}).get("kind")=="SignedBitVector" else z.ZeroExt(delta,value)
                return z.Extract(dest.size()-1,0,value)
            if z.is_bv(value) and dest==z.IntSort():return z.BV2Int(value,children[0].get("sort",{}).get("kind")=="SignedBitVector")
            if value.sort()==z.IntSort() and z.is_bv_sort(dest):return z.Int2BV(value,dest.size())
            raise Unavailable("unsupported cast")
        if op in ["&&","and"]:return z.And(*args)
        if op in ["||","or"]:return z.Or(*args)
        if op in ["!","not"]:return z.Not(args[0])
        if op=="implies":return z.Implies(*args)
        if op=="ite" or kind=="Ite":return z.If(*args)
        if op=="+":return sum(args)
        if op=="-":return -args[0] if len(args)==1 else args[0]-args[1]
        if op=="*":return args[0]*args[1]
        if op=="==":return args[0]==args[1]
        if op=="!=":return args[0]!=args[1]
        if op in ["<","<=",">",">="]:
            unsigned=children[0].get("sort",{}).get("kind")=="UnsignedBitVector"
            if unsigned:
                return {"<":z.ULT,"<=":z.ULE,">":z.UGT,">=":z.UGE}[op](*args)
            if op=="<":return args[0]<args[1]
            if op=="<=":return args[0]<=args[1]
            if op==">":return args[0]>args[1]
            return args[0]>=args[1]
        raise Unavailable("unsupported formula operation "+str((kind,op)))

    def check(self,formula):
        solver=self.z.Solver();solver.set(timeout=1000);solver.add(formula)
        start=time.monotonic();status=solver.check()
        evidence=dict(smt2=solver.to_smt2(),status=str(status),elapsed_ms=(time.monotonic()-start)*1000)
        if status==self.z.sat:evidence["model"]={str(d):str(solver.model()[d]) for d in solver.model().decls()}
        if status==self.z.unknown:evidence["reason"]=solver.reason_unknown()
        self.calls.append(evidence)
        return str(status),evidence


class Adapter:
    def __init__(self,manifest,mode):
        self.d=manifest;self.mode=mode
        self.sites={s["id"]:s for s in manifest["relay_sites"]}
        self.funcs={f["source_function_id"]:f for f in manifest["functions"]}
        self.cfg={f["function_id"]:f for f in manifest["control_flow"]["functions"]}
        self.handlers={h["id"]:h for h in manifest["handlers"]}
        self.cert={f["source_function_id"]:f for f in manifest["parallel_certificate"]["functions"]}
        self.formula=Formula() if mode in ["formula_smt","full"] else None
        self.path_cache={};self.resource_cache={}

    def resolve(self,entry):
        if "oracle_model" in entry:raise ValueError("oracle data must not enter inference")
        roots=[fid for fid,f in self.funcs.items() if f["source_function_signature"]==entry["function_signature"]]
        if len(roots)!=1:raise ValueError("entry does not resolve uniquely: "+entry["id"])
        sites={}
        for label,loc in entry["sites"].items():
            matches=[s for s in self.sites.values() if s["source_function_signature"]==loc["source_function_signature"]]
            if "line" in loc:matches=[s for s in matches if s["location"]["line"]==loc["line"]]
            if "target_text" in loc:
                matches=[s for s in matches if s.get("target",{}).get("text")==loc["target_text"]]
                n=loc.get("occurrence",0);matches=matches[n:n+1]
            if len(matches)!=1:raise ValueError("site does not resolve uniquely: %s/%s %s"%(entry["id"],label,loc))
            sites[label]=matches[0]["id"]
        return roots[0],sites

    def scan(self,root,q,sites):
        if "site_a" not in q:
            if not self.sites:return answer("bound",0,"no relay sites anywhere in this closed compilation unit","site_scan")
            return answer("unknown",reason="site collection has no invocation/loop or relay-tree bound",origin="site_scan")
        a,b=(self.sites[sites[q[x]]] for x in ["site_a","site_b"])
        if q["property"]=="may_alias":
            va,vb=literal(a),literal(b)
            if a["target_scope"]!=b["target_scope"]:
                return answer("decided",False,"different scope identities cannot alias","site_scan")
            if va is not None and vb is not None and va!=vb:
                return answer("decided",False,"different literal target keys","site_scan")
        # Opposite arms of exactly the same lexical branch are exclusive for
        # distinct, loop-free occurrences. Re-evaluated conditions are not equated.
        if q["property"] in ["may_alias","mutual_exclusion"] and not a["loops"] and not b["loops"]:
            for x in a["branches"]:
                for y in b["branches"]:
                    if x.get("condition",{}).get("location")==y.get("condition",{}).get("location") and x.get("polarity") is not None and x.get("polarity")!=y.get("polarity"):
                        return answer("decided",q["property"]=="mutual_exclusion","opposite arms of one loop-free lexical branch","site_scan")
        return answer("unknown",reason="syntax alone does not establish this trace property",origin="site_scan")

    def paths(self,root,smt=False,stack=()):
        key=(root,smt)
        if key in self.path_cache:return self.path_cache[key]
        if root in stack:raise Unavailable("recursive synchronous call")
        if root not in self.cfg:raise Unavailable("missing CFG")
        f=self.cfg[root];nodes={n["id"]:n for n in f["nodes"]};edges={n:[] for n in nodes}
        for edge in f["edges"]:
            if not edge.get("supported",False):raise Unavailable("unsupported CFG edge")
            edges[edge["source"]].append(edge)
        known_reasons={"relay emission may fail through the PREDA runtime"}
        initial_exact=not set(f.get("completeness_reasons",[]))-known_reasons
        paths=[]
        def visit(nodeid,events,condition,exact,seen):
            if len(paths)>4096:raise Unavailable("path enumeration exceeds 4096")
            if nodeid in seen:raise Unavailable("cyclic CFG needs occurrence-indexed analysis")
            n=nodes[nodeid]
            if not n.get("supported",False) or n["kind"]=="Opaque":raise Unavailable("unsupported CFG node")
            effect=n.get("direct_effect",{})
            if effect.get("may_call_unknown") or effect.get("may_have_external_effect"):exact=False
            variants=[(events,condition,exact,True)]
            if n["kind"]=="RelayEmit":variants=[(events+[n["relay_site_id"]],condition,exact,True)]
            if n["kind"]=="SynchronousCall":
                callee=n.get("callee_function_id")
                if not callee:raise Unavailable("unresolved synchronous call")
                children=self.paths(callee,smt,stack+(root,))
                variants=[]
                callee_graph=self.cfg[callee]
                safe_noarg=(self.funcs[callee]["source_function_signature"].endswith("()") and
                    all(cn["kind"] not in ["Branch","LoopHeader","SynchronousCall"] and
                        not any(cn.get("direct_effect",{}).get(k) for k in ["reads_current_scope_state","writes_current_scope_state","reads_global_state","writes_global_state","may_call_unknown","may_have_external_effect"])
                        for cn in callee_graph["nodes"]))
                for ev,cond,ex,returns in children:
                    # Formal/actual substitution is intentionally unavailable in
                    # this conventional Formula baseline. Structural composition
                    # remains usable; SAT existence is not asserted across calls.
                    merged=self.formula.z.And(condition,cond) if smt else None
                    variants.append((events+ev,merged,exact and ex and (safe_noarg or not smt),returns))
            outgoing=edges[nodeid]
            for events2,cond2,exact2,returns in variants:
                if not returns:
                    paths.append((events2,cond2,exact2,False));continue
                if not outgoing:
                    paths.append((events2,cond2,exact2,n["kind"]!="AbortOrFailure"));continue
                for edge in outgoing:
                    newcond,newexact=cond2,exact2
                    if smt and edge["kind"] in ["TrueBranch","FalseBranch"]:
                        try:guard=self.formula.convert(n["condition"])
                        except (Unavailable,KeyError):
                            guard=self.formula.z.Bool("unknown_condition::"+nodeid);newexact=False
                        if edge["kind"]=="FalseBranch":guard=self.formula.z.Not(guard)
                        newcond=self.formula.z.And(cond2,guard)
                    # Exceptional edges model failure prefixes; a path taking
                    # one cannot witness successful emission of the source site.
                    if edge["kind"]=="ExceptionalOrFailure":
                        prefix=events2[:-1] if n["kind"]=="RelayEmit" else events2
                        paths.append((prefix,newcond,False,False));continue
                    visit(edge["target"],events2,newcond,newexact,seen|{nodeid})
        initial=self.formula.z.BoolVal(True) if smt else None
        visit(f["entry_node_id"],[],initial,initial_exact,set())
        self.path_cache[key]=paths
        return paths

    def cfg_pair(self,root,q,sites):
        a,b=sites[q["site_a"]],sites[q["site_b"]]
        try:paths=self.paths(root)
        except Unavailable as exc:return answer("unknown",reason=str(exc),origin="cfg_icfg")
        co=[ev for ev,_,_,_ in paths if a in ev and b in ev]
        prop=q["property"]
        if prop=="must_precede":
            violations=[ev for ev,_,_,_ in paths if any(b==site and a not in ev[:i] for i,site in enumerate(ev))]
            if not violations:return answer("decided",True,"all over-approximated CFG/ICFG traces satisfy predecessor relation","cfg_icfg")
        if not co and prop in ["may_alias","mutual_exclusion"]:
            return answer("decided",prop=="mutual_exclusion","no CFG/ICFG path visits both sites","cfg_icfg")
        # Structural counterexamples with branch conditions are not concrete
        # witnesses. Only an unconditional supported path can justify a negative.
        f=self.cfg[root]
        def unconditional(fid,seen=()):
            if fid in seen:return False
            graph=self.cfg[fid]
            if set(graph.get("completeness_reasons",[])) - {"relay emission may fail through the PREDA runtime"}:return False
            for node in graph["nodes"]:
                if node["kind"] in ["Branch","LoopHeader"]:return False
                if any(node.get("direct_effect",{}).get(k) for k in ["may_call_unknown","may_have_external_effect"]):return False
                if node["kind"]=="SynchronousCall" and not unconditional(node["callee_function_id"],seen+(fid,)):return False
            return True
        exact=unconditional(root)
        if exact:
            if prop=="mutual_exclusion" and co:return answer("decided",False,"unconditional supported path emits both sites","cfg_icfg")
            if prop=="must_precede" and violations:return answer("decided",False,"unconditional path violates predecessor relation","cfg_icfg")
            if prop=="may_alias" and co:
                sa,sb=self.sites[a],self.sites[b]
                va,vb=literal(sa),literal(sb)
                if va is not None and vb is not None:return answer("decided",va==vb,"unconditional literal-target witness","cfg_icfg")
                if sa.get("target_dependency",{}).get("dependencies")==["TransactionArgument"] and sb.get("target_dependency",{}).get("dependencies")==["TransactionArgument"]:
                    if sa["target"]["kind"]==sb["target"]["kind"]=="identifier":
                        return answer("decided",True,"unconstrained input keys may take equal values","cfg_icfg")
        return answer("unknown",reason="CFG admits paths but does not decide their data feasibility",origin="cfg_icfg")

    def target(self,site):
        constraints=[c for c in self.d["refinement"]["constraints"] if c["kind"]=="RelayTargetRelation" and c.get("relay_site_id")==site]
        if len(constraints)!=1:raise Unavailable("missing target formula")
        formula=constraints[0]["formula"]
        if formula.get("operator")!="implies":raise Unavailable("unexpected target formula")
        equality=formula["children"][1]
        if equality.get("operator")!="==":raise Unavailable("unexpected target equality")
        return self.formula.convert(equality["children"][1])

    def smt_pair(self,root,q,sites):
        a,b=sites[q["site_a"]],sites[q["site_b"]];prop=q["property"]
        try:
            paths=self.paths(root,True)
            candidates=[]
            for events,condition,exact,_ in paths:
                relevant=(a in events and b in events) if prop!="must_precede" else any(site==b and a not in events[:i] for i,site in enumerate(events))
                if relevant:candidates.append((condition,exact))
            equality=self.formula.z.BoolVal(True)
            if prop=="may_alias":
                if self.sites[a]["target_scope"]!=self.sites[b]["target_scope"]:
                    return answer("decided",False,"different target scopes","formula_smt")
                equality=self.target(a)==self.target(b)
            formula=self.formula.z.Or(*[self.formula.z.And(cond,equality) for cond,_ in candidates])
            status,evidence=self.formula.check(formula)
            if status=="unsat":
                return answer("decided",prop!="may_alias","SMT excludes every violating/existential trace in the CFG over-approximation","formula_smt",solver=evidence)
            if status=="unknown":return answer("timeout" if "timeout" in evidence.get("reason","") else "unknown",reason=evidence.get("reason","solver unknown"),origin="formula_smt",solver=evidence)
            exact_formula=self.formula.z.Or(*[self.formula.z.And(cond,equality) for cond,exact in candidates if exact])
            exact_status,exact_evidence=self.formula.check(exact_formula)
            if exact_status=="sat":return answer("decided",prop=="may_alias","SMT witness on a supported exact acyclic path","formula_smt",solver=exact_evidence)
            return answer("unknown",reason="SAT only in an inexact path abstraction; no concrete witness asserted",origin="formula_smt",solver=evidence)
        except Unavailable as exc:return answer("unsupported",reason=str(exc),origin="formula_smt")

    def structural_resources(self,root,stack=()):
        if root in self.resource_cache:return self.resource_cache[root]
        if root in stack:raise Unavailable("cycle in relay/call graph; no decreasing measure")
        if root not in self.funcs:raise Unavailable("unresolved handler")
        f=self.funcs[root];summary=f["summary"]
        if summary.get("has_unmodeled_relay_reachable_call") or summary.get("has_opaque"):
            raise Unavailable("incomplete call/opaque summary")
        paths=None
        try:
            paths=self.paths(root)
            direct=max([0]+[len(events) for events,_,_,_ in paths])
            used=set(s for events,_,_,_ in paths for s in events)
        except Unavailable:
            direct=upper(summary["relay_count_upper_bound"])
            used=set(summary["relay_site_set"])
        if direct==0:return {"direct_work":0,"tree_work":0,"depth":0}
        child={}
        for sid in used:
            site=self.sites[sid];h=self.handlers.get(site["handler_id"],{})
            if not h.get("resolved"):raise Unavailable("unresolved relay handler")
            # A global/all-shard emission counts once in logical-work semantics.
            if h.get("relay_reachability_known") and not h.get("may_emit_relay"):
                child[sid]={"tree_work":0,"depth":0}
            else:child[sid]=self.structural_resources(h["target_function_id"],stack+(root,))
        if paths is not None:
            tree=max([0]+[sum(1+child[s]["tree_work"] for s in ev) for ev,_,_,_ in paths])
        else:tree=direct*max([1]+[1+c["tree_work"] for c in child.values()])
        depth=max([0]+[1+c["depth"] for c in child.values()])
        result=dict(direct_work=direct,tree_work=tree,depth=depth)
        self.resource_cache[root]=result;return result

    def cfg_resource(self,root,prop):
        try:
            if prop=="direct_work":
                try:value=max([0]+[len(e) for e,_,_,_ in self.paths(root)])
                except Unavailable:
                    summary=self.funcs[root]["summary"]
                    if summary.get("has_unmodeled_relay_reachable_call") or summary.get("has_opaque"):raise Unavailable("incomplete summary")
                    value=upper(summary["relay_count_upper_bound"])
            else:value=self.structural_resources(root)[prop]
            return answer("bound",value,"CFG path maximum / finite summary with conservative handler-graph composition","cfg_icfg")
        except Unavailable as exc:
            # Unknown loop multiplicity does not make acyclic relay depth unknown.
            if prop=="depth":
                try:
                    def depth(fid,stack=()):
                        if fid in stack:raise Unavailable("recursive relay/call graph")
                        graph=self.cfg[fid]
                        vals=[0]
                        for node in graph["nodes"]:
                            if not node.get("supported") or node["kind"]=="Opaque":raise Unavailable("unsupported depth CFG node")
                            if node.get("direct_effect",{}).get("may_call_unknown"):raise Unavailable("unknown call in depth closure")
                            if node["kind"]=="SynchronousCall":
                                callee=node.get("callee_function_id")
                                if not callee:raise Unavailable("unresolved sync call")
                                vals.append(depth(callee,stack+(fid,)))
                            if node["kind"]=="RelayEmit":
                                h=self.handlers[self.sites[node["relay_site_id"]]["handler_id"]]
                                if not h.get("resolved"):raise Unavailable("unresolved handler")
                                vals.append(1+(0 if h.get("relay_reachability_known") and not h.get("may_emit_relay") else depth(h["target_function_id"],stack+(fid,))))
                        return max(vals)
                    return answer("bound",depth(root),"acyclic handler-graph depth, independent of loop multiplicity","cfg_icfg")
                except Unavailable:pass
            return answer("unknown",reason=str(exc),origin="cfg_icfg")

    def smt_resource(self,root,prop):
        # Tighten root paths with SMT. Child maxima remain conservative and
        # independent of root input bindings, as in a conventional summary analysis.
        try:
            paths=self.paths(root,True);candidates=[];solvers=[]
            for events,cond,_,_ in paths:
                status,evidence=self.formula.check(cond);solvers.append(evidence)
                if status=="unsat":continue
                if prop=="direct_work":value=len(events)
                else:
                    weights=[]
                    for sid in events:
                        h=self.handlers[self.sites[sid]["handler_id"]]
                        if not h.get("resolved"):raise Unavailable("unresolved handler")
                        if h.get("relay_reachability_known") and not h.get("may_emit_relay"):weights.append(1)
                        else:weights.append(1+self.structural_resources(h["target_function_id"])[prop])
                    value=sum(weights) if prop=="tree_work" else max([0]+weights)
                candidates.append(value)
            return answer("bound",max([0]+candidates),"SMT-feasible CFG paths with conservative child bounds","formula_smt",solver_checks=solvers)
        except Unavailable as exc:return answer("unknown",reason=str(exc),origin="formula_smt")

    def native(self,root,q,sites):
        cert=self.cert.get(root)
        if not cert:return answer("unknown",reason="no native certificate for entry",origin="native_certificate")
        prop=q["property"]
        if "site_a" not in q:
            key={"direct_work":"direct_work_bound","tree_work":"transitive_work_bound","depth":"depth_bound"}[prop]
            c=cert.get(key,{})
            if c.get("status") in ["Complete","Conservative","Proved"]:
                try:return answer("bound",upper(c["upper_bound"]),c.get("reason",""),"native_certificate",certificate_id=c["certificate_id"])
                except (Unavailable,KeyError):pass
            return answer("unknown",reason=c.get("reason","no finite native bound"),origin="native_certificate")
        a,b=sites[q["site_a"]],sites[q["site_b"]]
        records=[c for c in cert["pair_relations"] if {c["site_a"],c["site_b"]}=={a,b}]
        for c in records:
            rel=c["relation"]
            if c.get("status")!="Proved":continue
            value=None
            if prop=="may_alias":
                if rel in ["CoEmissionIndependent","MutuallyExclusive"]:value=False
                elif rel=="ProvedMayAlias":value=True
            elif prop=="mutual_exclusion":
                if rel=="MutuallyExclusive":value=True
                elif rel in ["CoEmissionIndependent","ProvedMayAlias"]:value=False
            elif prop=="must_precede":
                if rel=="MustPrecedeAB" and (a,b)==(c["site_a"],c["site_b"]):value=True
                if rel=="MustPrecedeBA" and (b,a)==(c["site_a"],c["site_b"]):value=True
                # A reverse dominance certificate alone is not a reachable
                # counterexample: it may be vacuous. Leave False to a witness.
            if value is not None:return answer("decided",value,c["reason"],"native_certificate",certificate_id=c["certificate_id"],native_record=c)
        timeout=any("timeout" in str(c.get("solver_results",{})).lower() for c in records)
        return answer("timeout" if timeout else "unknown",reason="no proved native answer to this predicate",origin="native_certificate")

    def answer(self,entry,q):
        root,sites=self.resolve(entry)
        raw=self.native(root,q,sites) if self.mode=="full" else None
        if raw and raw["status"] in ["decided","unbounded"]:result=dict(raw)
        else:
            result=self.scan(root,q,sites)
            if self.mode!="site_scan" and result["status"] not in ["decided","bound"]:
                result=self.cfg_pair(root,q,sites) if "site_a" in q else self.cfg_resource(root,q["property"])
            if self.formula and (result["status"] not in ["decided","bound"] or "site_a" not in q):
                advanced=self.smt_pair(root,q,sites) if "site_a" in q else self.smt_resource(root,q["property"])
                if advanced["status"] in ["decided","bound"]:
                    if result["status"]!="bound" or advanced["value"]<result["value"]:result=advanced
                elif result["status"] not in ["decided","bound"]:result=advanced
        if raw is not None:
            if raw["status"]=="bound" and (result["status"]!="bound" or raw["value"]<=result["value"]):result=dict(raw)
            result["native_answer"]=raw
        result["query_semantics"]="direct-invocation trace / logical relay tree; no gas limit"
        return result
