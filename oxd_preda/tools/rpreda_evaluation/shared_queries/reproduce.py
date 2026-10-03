#!/usr/bin/env python3
"""Verify an exported bundle or replay its frozen corpus into a new directory."""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import shutil
import subprocess
import sys


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("action",choices=["verify","replay"])
    parser.add_argument("--bundle",type=pathlib.Path,default=pathlib.Path(__file__).resolve().parent.parent)
    parser.add_argument("--output",type=pathlib.Path)
    args=parser.parse_args();bundle=args.bundle.resolve()
    env=os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"]="1"
    env["PYTHONPATH"]=str(bundle/"python")+os.pathsep+str(bundle/"code")
    env["Z3_LIBRARY_PATH"]=str(bundle/"runtime")
    env["LD_LIBRARY_PATH"]=str(bundle/"runtime")+os.pathsep+env.get("LD_LIBRARY_PATH","")
    if args.action=="verify":
        checks=bundle/"SHA256SUMS"
        count=0
        for line in checks.read_text().splitlines():
            expected,relative=line.split("  ",1)
            path=bundle/relative
            if bundle not in path.resolve().parents:raise ValueError("checksum path escapes bundle")
            h=hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda:stream.read(1024*1024),b""):h.update(block)
            if h.hexdigest()!=expected:raise ValueError("checksum mismatch: "+relative)
            count+=1
        print("Verified %d retained files"%count,flush=True)
        env["RPREDA_SHARED_QUERY_RESULTS"]=str(bundle)
        subprocess.run([sys.executable,"-m","unittest","discover","-s",str(bundle/"code"),"-p","test_shared_queries.py","-v"],env=env,check=True)
        return
    if not args.output:parser.error("replay requires --output for a NEW directory")
    output=args.output.resolve()
    if output.exists():raise ValueError("replay output must not already exist")
    output.mkdir(parents=True)
    for name in ["corpus.json","oracle.json","FREEZE.json"]:shutil.copy2(str(bundle/name),str(output/name))
    shutil.copytree(str(bundle/"sources"),str(output/"sources"))
    base=[sys.executable,str(bundle/"code/run.py")]
    subprocess.run(base+["compile","--output",str(output),"--driver",str(bundle/"runtime/rpreda_analysis_driver"),"--library-path",str(bundle/"runtime"),"--repetitions","3"],env=env,check=True)
    subprocess.run(base+["answer","--output",str(output),"--repetitions","3"],env=env,check=True)
    subprocess.run(base+["report","--output",str(output)],env=env,check=True)
    print("Completed frozen-corpus replay: "+str(output))


if __name__=="__main__":main()
