#!/usr/bin/env python3
"""Reproducible shared-query runner; compile observations and answers are retained."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import random
import subprocess
import sys
import time

from corpus import build, digest
from oracle import evaluate

MODES = ["site_scan", "cfg_icfg", "formula_smt", "full"]


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def prepare(repo, output):
    corpus = build(repo, output)
    rows=[]
    for entry in corpus["entries"]:
        rows.extend(evaluate(entry,[q for q in corpus["queries"] if q["entry_id"]==entry["id"]]))
    save(output/"oracle.json",rows)
    save(output/"FREEZE.json",dict(frozen_at=datetime.datetime.utcnow().isoformat()+"Z",
        corpus_sha256=digest((output/"corpus.json").read_bytes()),
        oracle_sha256=digest((output/"oracle.json").read_bytes()),
        query_count=len(rows),source_count=len(corpus["programs"]),
        note="Frozen before mode results; reference models do not consume compiler outputs."))


def compile_all(args):
    output=args.output.resolve()
    corpus=json.loads((output/"corpus.json").read_text())
    driver=args.driver.resolve()
    binaries={str(driver):digest(driver.read_bytes()),str(driver.parent/"transpiler.so"):digest((driver.parent/"transpiler.so").read_bytes())}
    env=os.environ.copy()
    if args.library_path:env["LD_LIBRARY_PATH"]=":".join(args.library_path+[env.get("LD_LIBRARY_PATH","")])
    plan=[]
    for rep in range(args.repetitions):
        batch=[(p,m,rep) for p in corpus["programs"] for m in MODES]
        random.Random(865+rep).shuffle(batch)
        plan.extend(batch)
    save(output/"run_plan.json",dict(seed=865,repetitions=args.repetitions,modes=MODES,
        binaries=binaries,order=[dict(program=p["id"],mode=m,repetition=r) for p,m,r in plan],
        study="semantic effectiveness; timings descriptive, no warmup/performance claim"))
    failures=[]
    for i,(program,mode,rep) in enumerate(plan):
        dest=output/"runs"/program["id"]/mode/("rep_%02d"%rep)
        dest.mkdir(parents=True,exist_ok=True)
        source=output/program["path"]
        if digest(source.read_bytes())!=program["source_sha256"]:raise ValueError("source changed")
        existing=dest/"process.json"
        if args.resume and existing.exists():
            prior=json.loads(existing.read_text())
            if prior.get("source_sha256")==program["source_sha256"] and prior.get("binaries")==binaries:
                if prior["status"]!="ok":failures.append(prior)
                continue
            raise ValueError("stale retained run: "+str(dest))
        cmd=[str(driver),"--source",str(source),"--manifest",str(dest/"manifest.json"),
             "--metrics",str(dest/"metrics.json"),"--generated-cpp",str(dest/"generated.cpp"),
             "--dapp","SharedQueries","--profile","on","--analysis-mode",mode]
        start=time.monotonic()
        try:
            result=subprocess.run(cmd,cwd=str(driver.parent),env=env,stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,timeout=args.timeout)
            (dest/"stdout.log").write_bytes(result.stdout);(dest/"stderr.log").write_bytes(result.stderr)
            status="ok" if result.returncode==0 else "execution_error"
            returncode=result.returncode
        except subprocess.TimeoutExpired as exc:
            (dest/"stdout.log").write_bytes(exc.stdout or b"");(dest/"stderr.log").write_bytes(exc.stderr or b"")
            status,returncode="timeout",None
        record=dict(program=program["id"],mode=mode,repetition=rep,status=status,returncode=returncode,
                    process_ms=(time.monotonic()-start)*1000,command=cmd,
                    source_sha256=program["source_sha256"],binaries=binaries)
        if status=="ok":
            metrics=json.loads((dest/"metrics.json").read_text())
            if metrics.get("analysis_mode")!=mode:raise ValueError("mode mismatch")
            features=metrics["build_features"]
            if not features["z3"] or features["runtime_trace"] or features["runtime_optimization"]:raise ValueError("incorrect build features")
            record["build_features"]=features
            record["generated_cpp_sha256"]=digest((dest/"generated.cpp").read_bytes())
        else:failures.append(record)
        save(existing,record)
        print("[%d/%d] %s %s rep=%d %s"%(i+1,len(plan),program["id"],mode,rep,status),flush=True)
    save(output/"compile_status.json",dict(total=len(plan),failures=failures,complete=True))
    return not failures


def answer_all(args):
    from adapters import Adapter
    output=args.output.resolve();corpus=json.loads((output/"corpus.json").read_text())
    freeze=json.loads((output/"FREEZE.json").read_text())
    for filename,key in [("corpus.json","corpus_sha256"),("oracle.json","oracle_sha256")]:
        if digest((output/filename).read_bytes())!=freeze[key]:raise ValueError("frozen input changed")
    rows=[]
    for program in corpus["programs"]:
        entries=[e for e in corpus["entries"] if e["program_id"]==program["id"]]
        # The inference API receives only public queries and locators, never reference models.
        public=[{k:v for k,v in e.items() if k!="oracle_model"} for e in entries]
        qs=[q for q in corpus["queries"] if q["entry_id"] in {e["id"] for e in public}]
        for mode in MODES:
            for rep in range(args.repetitions):
                dest=output/"runs"/program["id"]/mode/("rep_%02d"%rep)
                process=json.loads((dest/"process.json").read_text())
                adapter=Adapter(json.loads((dest/"manifest.json").read_text()),mode) if process["status"]=="ok" else None
                current=[]
                for q in qs:
                    entry=next(e for e in public if e["id"]==q["entry_id"])
                    answer=adapter.answer(entry,q) if adapter else dict(status=process["status"],value=None,reason="compiler process did not complete",origin="process")
                    current.append(dict(query_id=q["id"],mode=mode,repetition=rep,**answer))
                save(dest/"answers.json",current);rows.extend(current)
        print("answered "+program["id"],flush=True)
    save(output/"answers.json",rows)
    return rows


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("phase",choices=["prepare","compile","answer","report","all"])
    parser.add_argument("--repo",type=pathlib.Path,default=pathlib.Path(__file__).resolve().parents[4])
    parser.add_argument("--output",type=pathlib.Path,required=True)
    parser.add_argument("--driver",type=pathlib.Path,default=pathlib.Path("bin/bin_release/rpreda_analysis_driver"))
    parser.add_argument("--library-path",action="append",default=[])
    parser.add_argument("--repetitions",type=int,default=3)
    parser.add_argument("--timeout",type=int,default=120)
    parser.add_argument("--resume",action="store_true")
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    if args.phase in ["prepare","all"]:prepare(args.repo,args.output)
    if args.phase in ["compile","all"]:
        if not compile_all(args):sys.exit(1)
    if args.phase in ["answer","all"]:answer_all(args)
    if args.phase in ["report","all"]:
        from report import report
        report(args.output)


if __name__=="__main__":main()
