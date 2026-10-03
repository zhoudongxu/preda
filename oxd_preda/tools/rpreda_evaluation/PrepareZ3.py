#!/usr/bin/env python3
"""Install the C/C++ payload of a local z3-solver wheel into a stable prefix.

No Python-package import, network access, or system-wide installation is needed.
Existing files are accepted only if they contain the same bytes.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import zipfile


def prepare(wheel: pathlib.Path, prefix: pathlib.Path) -> dict:
    files = {}
    with zipfile.ZipFile(wheel) as archive:
        for member in archive.infolist():
            parts = pathlib.PurePosixPath(member.filename).parts
            if len(parts) < 3 or parts[:2] not in (("z3", "include"), ("z3", "lib")) or member.is_dir():
                continue
            if ".." in parts or "\\" in member.filename:
                raise ValueError("unsafe wheel path: " + member.filename)
            relative = pathlib.Path(*parts[1:])
            contents = archive.read(member)
            target = prefix / relative
            if target.is_symlink() or (target.exists() and target.read_bytes() != contents):
                raise ValueError("refusing to replace different existing dependency: " + str(target))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(contents)
            files[relative.as_posix()] = hashlib.sha256(contents).hexdigest()
        for member in archive.infolist():
            if ".dist-info/" in member.filename and "license" in member.filename.lower() and not member.is_dir():
                target = prefix / "LICENSE.txt"
                target.write_bytes(archive.read(member))
                files[target.name] = hashlib.sha256(target.read_bytes()).hexdigest()
                break
    for required in ("include/z3++.h", "include/z3.h", "lib/libz3.so"):
        if required not in files:
            raise ValueError("wheel lacks required Linux C/C++ payload: " + required)
    digest = hashlib.sha256()
    with wheel.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1048576), b""):
            digest.update(chunk)
    record = {"wheel_file": wheel.name, "wheel_sha256": digest.hexdigest(),
              "prefix": str(prefix), "files_sha256": files,
              "cmake_definitions": {"RPREDA_ENABLE_Z3": "ON",
                  "RPREDA_Z3_INCLUDE_DIR": str(prefix / "include"),
                  "RPREDA_Z3_LIBRARY": str(prefix / "lib/libz3.so")}}
    (prefix / "dependency.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wheel", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    args = parser.parse_args()
    record = prepare(args.wheel.resolve(), args.prefix.resolve())
    print(json.dumps({"prefix": record["prefix"], "cmake_definitions": record["cmake_definitions"], "wheel_sha256": record["wheel_sha256"]}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
