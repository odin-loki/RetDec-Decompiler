#!/usr/bin/env python3
"""Decompile small cross-compiled fixtures and check the emitted C.

Usage: arch_fixtures_test.py <decompiler> <fixtures_dir> <work_dir>
A "!" prefix on an expectation means the substring must NOT appear.
"""
import json
import os
import subprocess
import sys


def main():
    dec, fdir, work = sys.argv[1:4]
    os.makedirs(work, exist_ok=True)
    with open(os.path.join(fdir, "expectations.json")) as fh:
        exp = json.load(fh)
    env = dict(os.environ, RETDEC_INCREMENTAL_CACHE="0")
    failures = 0
    for name, checks in sorted(exp.items()):
        if name.startswith("_"):
            continue
        out = os.path.join(work, name + ".c")
        if os.path.exists(out):
            os.remove(out)
        r = subprocess.run([dec, os.path.join(fdir, name), "-o", out],
                           capture_output=True, text=True, env=env, timeout=300)
        if r.returncode != 0 or not os.path.exists(out):
            print(f"FAIL {name}: exit {r.returncode}\n{r.stderr[-2000:]}")
            failures += 1
            continue
        text = open(out, encoding="utf-8", errors="replace").read()
        for c in checks:
            bad = c[1:] in text if c.startswith("!") else c not in text
            if bad:
                print(f"FAIL {name}: expectation {c!r}")
                failures += 1
        print(f"ok   {name}")
    if failures:
        print(f"{failures} failure(s)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
