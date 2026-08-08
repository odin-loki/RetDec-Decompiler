#!/usr/bin/env python3
"""DecompileBench runner (arXiv 2505.11340 scaffold)."""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path


def try_recompile(source_c: Path, cc: str) -> bool | None:
    if not shutil.which(cc):
        return None
    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / "a.out"
        proc = subprocess.run(
            [cc, "-O2", "-o", str(exe), str(source_c)],
            capture_output=True,
            text=True,
        )
        return proc.returncode == 0


def run_decompiler(binary: Path, input_bin: Path, opt: str, out_dir: Path, cc: str) -> dict:
    out_c = out_dir / f"{input_bin.stem}-{opt}.c"
    t0 = time.time()
    proc = subprocess.run(
        [str(binary), str(input_bin), "--output", str(out_c)],
        capture_output=True,
        text=True,
    )
    elapsed = time.time() - t0
    syntax_valid = out_c.exists() and out_c.stat().st_size > 0
    recompile = try_recompile(out_c, cc) if syntax_valid else None
    return {
        "input": str(input_bin),
        "opt": opt,
        "exit_code": proc.returncode,
        "wall_s": elapsed,
        "output_c": str(out_c),
        "syntax_valid": syntax_valid,
        "recompile_success": recompile,
        "coverage_equivalence": None,
        "stderr_tail": (proc.stderr or "")[-500:],
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decompiler", required=True, help="path to retdec-decompiler")
    ap.add_argument("--corpus", required=True, help="directory of test binaries")
    ap.add_argument("--out", default="results/decompilebench.json")
    ap.add_argument("--opts", nargs="+", default=["O0", "O2", "O3"])
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()

    binary = Path(args.decompiler)
    corpus = Path(args.corpus)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    for sample in sorted(corpus.glob("*")):
        if not sample.is_file():
            continue
        for opt in args.opts:
            rows.append(run_decompiler(binary, sample, opt, out_path.parent / "artifacts", args.cc))

    payload = {
        "harness": "decompilebench",
        "decompiler": str(binary),
        "cc": args.cc,
        "samples": rows,
        "summary": {
            "count": len(rows),
            "syntax_valid": sum(1 for r in rows if r["syntax_valid"]),
            "recompile_ok": sum(1 for r in rows if r["recompile_success"] is True),
        },
    }
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
