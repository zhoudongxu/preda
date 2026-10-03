"""Fixed source programs and independent reference models, frozen before analysis."""
from __future__ import annotations

import hashlib
import itertools
import json
import pathlib
import re


def digest(data):
    return hashlib.sha256(data if isinstance(data, bytes) else data.encode()).hexdigest()


def emit(label, target, children=None, scope="uint32"):
    return ["emit", label, scope, target, children or []]


def conditional(condition, yes, no=None):
    return ["if", condition, yes, no or []]


def model(program, domains=None, justification=None, **extra):
    return dict(program=program, domains=domains or {},
                justification=justification or (
                    "Enumerate every Boolean valuation and every equality partition of input keys. "
                    "Keys are used only for equality, so renaming preserves all queried properties. "
                    "The model lists all successful emissions; failure prefixes cannot increase "
                    "work/depth, introduce alias/coemission, or violate an established predecessor."), **extra)


def controlled():
    a, b = emit("A", "a"), emit("B", "b")
    keys = {"a": [0, 1, 2], "b": [0, 1, 2], "c": [0, 1, 2]}
    common = "uint32 a, uint32 b, uint32 c, bool p, bool q"
    examples = [
        ("literal_distinct", "relay@(0u32) leaf(); // Q:A\nrelay@(1u32) leaf(); // Q:B", [emit("A", 0), emit("B", 1)], {}),
        ("literal_alias", "relay@(0u32) leaf(); // Q:A\nrelay@(0u32) leaf(); // Q:B", [emit("A", 0), emit("B", 0)], {}),
        ("input_alias", "relay@a leaf(); // Q:A\nrelay@b leaf(); // Q:B", [a, b], keys),
        ("guard_distinct", "if(a != b) {\nrelay@a leaf(); // Q:A\nrelay@b leaf(); // Q:B\n}", [conditional("a != b", [a,b])], keys),
        ("guard_equal", "if(a == b) {\nrelay@a leaf(); // Q:A\nrelay@b leaf(); // Q:B\n}", [conditional("a == b", [a,b])], keys),
        ("transitive_guard", "if(a != b && b == c) {\nrelay@a leaf(); // Q:A\nrelay@c leaf(); // Q:B\n}", [conditional("a != b and b == c", [a,emit("B","c")])], keys),
        ("negated_guard", "if(!(a == b)) {\nrelay@a leaf(); // Q:A\nrelay@b leaf(); // Q:B\n}", [conditional("not (a == b)", [a,b])], keys),
        ("if_else", "if(p) {\nrelay@a leaf(); // Q:A\n} else {\nrelay@b leaf(); // Q:B\n}", [conditional("p",[a],[b])], dict(keys,p=[False,True])),
        ("complementary_if", "if(p) {\nrelay@a leaf(); // Q:A\n}\nif(!p) {\nrelay@b leaf(); // Q:B\n}", [conditional("p",[a]),conditional("not p",[b])], dict(keys,p=[False,True])),
        ("independent_if", "if(p) {\nrelay@a leaf(); // Q:A\n}\nif(q) {\nrelay@b leaf(); // Q:B\n}", [conditional("p",[a]),conditional("q",[b])], dict(keys,p=[False,True],q=[False,True])),
        ("optional_predecessor", "if(p) {\nrelay@a leaf(); // Q:A\n}\nrelay@b leaf(); // Q:B", [conditional("p",[a]),b], dict(keys,p=[False,True])),
        ("early_return", "if(p) { return; }\nrelay@a leaf(); // Q:A\nrelay@b leaf(); // Q:B", [conditional("p",[["return"]]),a,b], dict(keys,p=[False,True])),
        ("boolean_update", "if(p) {\nrelay@a leaf(); // Q:A\n}\np = !p;\nif(p) {\nrelay@b leaf(); // Q:B\n}", [conditional("p",[a]),["set","p","not p"],conditional("p",[b])], dict(keys,p=[False,True])),
        ("local_copy", "uint32 k = a;\nif(k != b) {\nrelay@k leaf(); // Q:A\nrelay@b leaf(); // Q:B\n}", [["set","k","a"],conditional("k != b",[emit("A","k"),b])], keys),
        ("local_update", "uint32 k = a;\nrelay@k leaf(); // Q:A\nk = b;\nrelay@k leaf(); // Q:B", [["set","k","a"],emit("A","k"),["set","k","b"],emit("B","k")], keys),
        ("dead_branch", "if(p && !p) {\nrelay@a leaf(); // Q:A\n}\nrelay@b leaf(); // Q:B", [conditional("p and not p",[a]),b], dict(keys,p=[False,True])),
        ("fixed_loop", "for(uint32 i=0u32; i<3u32; i++) {\nrelay@a leaf(); // Q:A\n}\nrelay@b leaf(); // Q:B", [["repeat",3,[a]],b], keys),
        ("nested_loop", "for(uint32 i=0u32; i<2u32; i++) {\nfor(uint32 j=0u32; j<3u32; j++) {\nrelay@a leaf(); // Q:A\n}\n}", [["repeat",6,[a]]], keys),
        ("zero_loop", "for(uint32 i=0u32; i<0u32; i++) {\nrelay@a leaf(); // Q:A\n}\nrelay@b leaf(); // Q:B", [b], keys),
        ("no_relay", "uint32 k = a;", [], keys),
    ]
    out = []
    for name, body, program, domains in examples:
        source = ("contract SQ_%s {\n@uint32 uint32 observed;\n"
                  "@uint32 function leaf() { observed = 1u32; }\n"
                  "@address function root(%s) export {\n%s\n}\n}\n") % (name, common, body)
        out.append((name,source,"root(uint32,uint32,uint32,bool,bool)",model(program,domains)))
    source = """contract SQ_uint8_boundary {
@uint32 uint32 observed;
@uint32 function leaf() { observed = 1u32; }
@address function root(uint8 x) export {
if(x < 255u8) {
relay@(uint32(x)) leaf(); // Q:A
relay@(uint32(x + 1u8)) leaf(); // Q:B
}
}
}
"""
    out.append(("uint8_boundary",source,"root(uint8)",model(
        [conditional("x < 255",[emit("A","x"),emit("B","x + 1")])],{"x":list(range(256))},
        "Exhaustively enumerate all 256 uint8 values. The guard prevents overflow; the uint32 casts preserve each value.")))
    source = """contract SQ_sync_helper {
@uint32 uint32 observed;
@uint32 function leaf() { observed = 1u32; }
@address function helper() {
relay@(0u32) leaf(); // Q:A
}
@address function root() export {
helper();
relay@(1u32) leaf(); // Q:B
}
}
"""
    out.append(("sync_helper",source,"root()",model([emit("A",0),emit("B",1)])))
    for name, body, program in [
        ("sync_repeat","helper();\nhelper();",[emit("A",0),emit("A",0)]),
        ("async_chain","relay@(0u32) middle(); // Q:A",[emit("A",0,[emit("child",1)])]),
        ("async_fanout","relay@(0u32) middle(); // Q:A\nrelay@(1u32) middle(); // Q:B",[emit("A",0,[emit("child",1),emit("child2",2)]),emit("B",1,[emit("child",1),emit("child2",2)])]),
    ]:
        middle = "relay@(1u32) leaf();"
        if name == "async_fanout": middle += "\nrelay@(2u32) leaf();"
        helper = "@address function helper() {\nrelay@(0u32) leaf(); // Q:A\n}\n" if name=="sync_repeat" else ""
        source = ("contract SQ_%s {\n@uint32 uint32 observed;\n@uint32 function leaf() { observed = 1u32; }\n"
                  "@uint32 function middle() {\n%s\n}\n%s@address function root() export {\n%s\n}\n}\n")%(name,middle,helper,body)
        out.append((name,source,"root()",model(program)))
    for name, source, program, unbounded, reason in [
        ("unbounded_loop", """contract SQ_unbounded_loop {
@uint32 uint32 observed;
@uint32 function leaf() { observed = 1u32; }
@address function root(bool p) export {
while(p) {
relay@(0u32) leaf(); // Q:A
}
}
}
""",[emit("A",0)],["direct_work","tree_work"],"With p=true, each successful iteration returns to the same loop header. Arbitrarily long finite prefixes emit arbitrarily many relays. All handlers are leaves, so depth is one."),
        ("recursive_relay", """contract SQ_recursive_relay {
@uint32 function again() {
relay@(0u32) again();
}
@address function root() export {
relay@(0u32) again(); // Q:A
}
}
""",[emit("A",0)],["tree_work","depth"],"The root emits one relay. Each handler emits one successor of the same function with the same key, with no guard or decreasing state. Arbitrarily long relay-tree prefixes exist."),
    ]:
        signature="root(bool)" if name=="unbounded_loop" else "root()"
        out.append((name,source,signature,model(program,unbounded=unbounded,unbounded_reason=reason)))
    return out


def marked_sites(source, root_signature):
    result = {}
    function = root_signature
    for lineno, line in enumerate(source.splitlines(),1):
        if "function helper()" in line: function="helper()"
        elif "function root(" in line: function=root_signature
        hit=re.search(r"// Q:(\w+)",line)
        if hit:
            result[hit.group(1)]={"source_function_signature":function,"line":lineno}
    return result


def real_specs():
    address="address"
    def e(label,target): return emit(label,target,scope=address)
    keys={"to":[0,1,2,3],"audit":[0,1,2,3],"enabled":[False,True]}
    token=model([conditional("enabled and to != audit",[e("A","to"),e("B","audit")])],keys,
        "The numeric guard amount>=0 && balance>=amount is abstracted by enabled. Both values are realizable independently of address equality (amount=0,balance=0 or amount=-1). The intervening bigint debit changes neither addresses nor emission control.")
    kitty_keys={k:[0,1,2] for k in ["offspring","parent1","parent2"]}
    kitty=model([conditional("offspring != parent1 and offspring != parent2 and parent1 != parent2",[e("A","offspring"),e("B","parent1"),e("C","parent2")])],kitty_keys,
        "Three address representatives cover every equality partition of three addresses. genes and gender affect only leaf-handler state, not relay control, keys, or further emissions.")
    ballot_keys={k:[0,1,2,3] for k in ["voter","statistics","yesStatistics","noStatistics"]}
    ballot=model([conditional("voter != statistics",[e("A","voter"),e("B","statistics")]),conditional("choice",[e("C","yesStatistics")]),conditional("not choice",[e("D","noStatistics")])],dict(ballot_keys,choice=[False,True]),
        "Four address representatives cover every equality partition of the four address inputs; enumerate both choice values. amount changes only leaf-handler counters, not relay emissions.")
    pixel=model([conditional("ownerStatisticsKey != index",[emit("A","ownerStatisticsKey"),emit("B","index")])],{"ownerStatisticsKey":[0,1],"index":[0,1]},
        "For uint16 x,y, index=x*65536+y is a bijection onto uint32 and never overflows. Thus the two uint32 keys have exactly equal/distinct cases; two representatives suffice. sender does not affect emissions.")
    air=model([conditional("enabled",[e("A","recipient0"),e("B","recipient1"),e("C","recipient2"),e("D","summary")])],dict({k:[0,1,2,3] for k in ["recipient0","recipient1","recipient2","summary"]},enabled=[False,True]),
        "All target address equality partitions are represented. The amount/total/balance guard has both outcomes independently of keys (all amounts zero and balance zero, or a negative amount). Audit/recipient alias is permitted by this source guard.")
    return [
        ("Token","transfer_parallel(address,address,bigint)",["to","audit"],token),
        ("Ballot","vote_parallel(bool,address,address,address,address,uint64)",["voter","statistics","yesStatistics","noStatistics"],ballot),
        ("MillionPixel","occupy_parallel(uint16,uint16,uint32,address)",["ownerStatisticsKey","index"],pixel),
        ("Kitty","breed_parallel(address,address,address,bigint,bool)",["offspring","parent1","parent2"],kitty),
        ("Token","transfer_n_parallel(address,bigint,address,bigint,address,bigint,address)",["recipient0","recipient1","recipient2","summary"],air),
    ]


def build(repo, output):
    output=pathlib.Path(output); (output/"sources").mkdir(parents=True,exist_ok=True)
    programs, entries=[],[]
    def add_source(pid,source,cohort,origin):
        path="sources/%s.prd"%pid
        (output/path).write_text(source,encoding="utf-8")
        programs.append(dict(id=pid,path=path,source_sha256=digest(source),cohort=cohort,origin=origin))
        return pid
    def add_entry(eid,pid,signature,sites,ref,cohort):
        entries.append(dict(id=eid,program_id=pid,function_signature=signature,sites=sites,oracle_model=ref,cohort=cohort))
    for name,source,signature,ref in controlled():
        pid=add_source("controlled_"+name,source,"controlled","fixed mechanism template")
        add_entry(pid,pid,signature,marked_sites(source,signature),ref,"controlled")
    specs=real_specs()
    originals={}
    for contract in ["Token","Ballot","MillionPixel","Kitty"]:
        origin="oxd_preda/simulator/contracts/%s.prd"%contract
        originals[contract]=pathlib.Path(repo,origin).read_text(encoding="utf-8")
        add_source("real_"+contract,originals[contract],"existing_contract",origin)
    for contract,sig,targets,ref in specs:
        sites={chr(65+i):{"source_function_signature":sig,"target_text":target,"occurrence":0} for i,target in enumerate(targets)}
        add_entry("real_"+contract+"_"+sig.split('(')[0],"real_"+contract,sig,sites,ref,"existing_extension")
    # Original entry points are retained separately from authored extensions.
    for contract,sig,target,ref in [
        ("Token","transfer(address,bigint)","to",model([conditional("enabled",[emit("A","to",scope="address")])],{"enabled":[False,True],"to":[0]},"The balance>=amount guard can be true or false; for the emission maximum choose amount=0,balance=0. The inline handler emits no relay.")),
        ("MillionPixel","occupy(uint16,uint16)","index",model([emit("A",0)],justification="The uint16 coordinate calculation is total and bounded by 2^32-1; one relay is unconditional and its handler has no further relay.")),
        ("Kitty","mint(bigint,bool,address)","owner",model([emit("A",0,scope="address")],justification="On successful create() and emission, mint emits one relay to owner. create and the inline relay handler emit no relays. Earlier failures only reduce work.")),
        ("Ballot","vote(uint32,uint32)",None,model([],justification="The source has no active relay in vote; its commented relay is not executable. It calls no relay-emitting helper.")),
    ]:
        sites={"A":{"source_function_signature":sig,"target_text":target,"occurrence":0}} if target else {}
        add_entry("original_"+contract,"real_"+contract,sig,sites,ref,"existing_original")
    # Two predetermined mutations of four extension slices. No outcome filtering.
    for contract,sig,targets,ref in specs[:4]:
        source=originals[contract]
        for mutation in ["alias_target","swap_order"]:
            lines=source.splitlines(keepends=True)
            indexes=[i for i,line in enumerate(lines) if re.search(r"relay@\s*"+re.escape(targets[0])+r"\b",line)]
            # Match within the chosen function's source suffix, not earlier functions.
            start=re.search(r"function\s+(?:bool\s+)?"+re.escape(sig.split('(')[0])+r"\s*\(",source).start()
            candidates=[i for i in indexes if sum(len(s) for s in lines[:i])>start]
            if len(candidates)!=1: raise ValueError((contract,"first site",candidates))
            ia=candidates[0]
            ib=next(i for i in range(ia+1,len(lines)) if re.search(r"relay@\s*"+re.escape(targets[1])+r"\b",lines[i]))
            newref=json.loads(json.dumps(ref))
            def mutate_program(program):
                for stmt in program:
                    if stmt[0]=="emit" and stmt[1]=="B" and mutation=="alias_target": stmt[3]=targets[0]
                    if stmt[0]=="if":
                        mutate_program(stmt[2]); mutate_program(stmt[3])
                if mutation=="swap_order":
                    positions={s[1]:i for i,s in enumerate(program) if s[0]=="emit"}
                    if "A" in positions and "B" in positions:
                        x,y=positions["A"],positions["B"];program[x],program[y]=program[y],program[x]
            mutate_program(newref["program"])
            if mutation=="alias_target": lines[ib]=lines[ib].replace("relay@"+targets[1],"relay@"+targets[0],1)
            else: lines[ia],lines[ib]=lines[ib],lines[ia]
            newsource="".join(lines)
            pid=add_source("mutant_"+contract+"_"+mutation,newsource,"source_mutant","real_"+contract+" / "+mutation)
            # Line locations preserve the A/B semantic identity after swapping.
            sites={chr(65+i):{"source_function_signature":sig,"target_text":t,"occurrence":0} for i,t in enumerate(targets)}
            sites["A"]={"source_function_signature":sig,"line":(ib if mutation=="swap_order" else ia)+1}
            sites["B"]={"source_function_signature":sig,"line":(ia if mutation=="swap_order" else ib)+1}
            newref["justification"] += " Source mutation: "+mutation+"; the reference trace model applies exactly that target/order edit."
            add_entry(pid,pid,sig,sites,newref,"source_mutant")
    queries=[]
    for entry in entries:
        eid=entry["id"]
        for a,b in itertools.combinations(sorted(entry["sites"]),2):
            for prop,x,y in [("may_alias",a,b),("mutual_exclusion",a,b),("must_precede",a,b),("must_precede",b,a)]:
                queries.append(dict(id=eid+"/"+prop+"/"+x+"/"+y,entry_id=eid,property=prop,site_a=x,site_b=y,cohort=entry["cohort"]))
        for prop in ["direct_work","tree_work","depth"]:
            queries.append(dict(id=eid+"/"+prop,entry_id=eid,property=prop,cohort=entry["cohort"]))
    corpus=dict(schema_version=1,programs=programs,entries=entries,queries=queries)
    (output/"corpus.json").write_text(json.dumps(corpus,indent=2,sort_keys=True)+"\n")
    return corpus
