#!/usr/bin/env python3
"""DecompileBench runner (arXiv 2505.11340).

Local stand-in corpus: tests/decompilebench/corpus/ (from fetch_decompilebench_corpus.sh).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def load_manifest(corpus: Path) -> list[dict]:
    manifest = corpus / "manifest.json"
    if not manifest.is_file():
        return []
    return json.loads(manifest.read_text(encoding="utf-8"))


def list_samples(corpus: Path, manifest: list[dict], limit: int | None) -> list[dict]:
    if manifest:
        items = manifest
    else:
        items = [
            {"name": p.name, "path": str(p), "opt": parse_opt(p.name)}
            for p in sorted(corpus.iterdir())
            if p.is_file() and p.name != "manifest.json" and p.name != "meta.json"
        ]
    if limit is not None:
        items = items[:limit]
    return items


def parse_opt(name: str) -> str:
    m = re.search(r"-(O\d)$", name, re.IGNORECASE)
    return m.group(1).upper() if m else "unknown"


def compile_flags_for_source(source_text: str) -> list[str]:
    flags = ["-std=c11"]
    if "pthread" in source_text:
        flags.append("-pthread")
    return flags


def try_compile(source_c: Path, cc: str, opt: str = "O2", extra: list[str] | None = None) -> tuple[Path | None, str]:
    if not shutil.which(cc):
        return None, "cc missing"
    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / "a.out"
        cmd = [cc, f"-{opt}", "-o", str(exe), str(source_c)]
        if extra:
            cmd.extend(extra)
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return None, (proc.stderr or proc.stdout or "")[-400:]
        return exe, ""


def run_binary(exe: Path, timeout: float = 5.0) -> tuple[int | None, str, str]:
    try:
        proc = subprocess.run(
            [str(exe)],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return None, "", "timeout"
    except OSError as exc:
        return None, "", str(exc)


def peak_rss_kb() -> int | None:
    if sys.platform != "linux":
        return None
    # ru_maxrss is KiB on Linux.
    return int(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)


def coverage_equivalence(
    orig_bin: Path,
    source_path: Path | None,
    dec_c: Path,
    cc: str,
    compile_opt: str,
) -> bool | None:
    orig_code, orig_out, orig_err = run_binary(orig_bin)
    if orig_code is None:
        return None

    extra: list[str] = []
    if source_path and source_path.is_file():
        extra = compile_flags_for_source(source_path.read_text(encoding="utf-8", errors="replace"))

    dec_exe, _ = try_compile(dec_c, cc, compile_opt, extra)
    if dec_exe is None:
        return False

    dec_code, dec_out, dec_err = run_binary(dec_exe)
    if dec_code is None:
        return None
    return orig_code == dec_code and orig_out == dec_out


def run_decompiler(
    decompiler: Path,
    input_bin: Path,
    opt: str,
    out_dir: Path,
    cc: str,
    source_path: Path | None,
    compile_opt: str,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    out_c = out_dir / f"{input_bin.name}-{opt}.c"
    t0 = time.time()
    proc = subprocess.run(
        [str(decompiler), str(input_bin), "--output", str(out_c)],
        capture_output=True,
        text=True,
        timeout=600,
    )
    elapsed = time.time() - t0
    rss = peak_rss_kb()
    syntax_valid = out_c.is_file() and out_c.stat().st_size > 0
    recompile = None
    cov = None
    if syntax_valid:
        extra = []
        if source_path and source_path.is_file():
            extra = compile_flags_for_source(
                source_path.read_text(encoding="utf-8", errors="replace")
            )
        dec_exe, _ = try_compile(out_c, cc, compile_opt, extra)
        recompile = dec_exe is not None
        if recompile:
            cov = coverage_equivalence(input_bin, source_path, out_c, cc, compile_opt)
    return {
        "input": str(input_bin),
        "opt": opt,
        "exit_code": proc.returncode,
        "wall_s": round(elapsed, 3),
        "peak_rss_kb": rss,
        "output_c": str(out_c),
        "syntax_valid": syntax_valid,
        "recompile_success": recompile,
        "coverage_equivalence": cov,
        "stderr_tail": (proc.stderr or "")[-500:],
    }


def summarize(rows: list[dict]) -> dict:
    def rate(key: str) -> float | None:
        vals = [r.get(key) for r in rows if r.get(key) is not None]
        if not vals:
            return None
        return sum(1 for v in vals if v) / len(vals)

    by_opt: dict[str, list[dict]] = {}
    for row in rows:
        by_opt.setdefault(row.get("opt", "unknown"), []).append(row)

    per_opt = {}
    for opt, group in sorted(by_opt.items()):
        per_opt[opt] = {
            "count": len(group),
            "syntax_valid_rate": rate_from(group, "syntax_valid"),
            "recompile_success_rate": rate_from(group, "recompile_success"),
            "coverage_equivalence_rate": rate_from(group, "coverage_equivalence"),
        }

    return {
        "count": len(rows),
        "syntax_valid": sum(1 for r in rows if r.get("syntax_valid")),
        "recompile_ok": sum(1 for r in rows if r.get("recompile_success") is True),
        "coverage_ok": sum(1 for r in rows if r.get("coverage_equivalence") is True),
        "syntax_valid_rate": rate_from(rows, "syntax_valid"),
        "recompile_success_rate": rate_from(rows, "recompile_success"),
        "coverage_equivalence_rate": rate_from(rows, "coverage_equivalence"),
        "per_opt": per_opt,
        "mean_wall_s": round(sum(r.get("wall_s", 0.0) for r in rows) / len(rows), 3) if rows else 0.0,
    }


def rate_from(rows: list[dict], key: str) -> float | None:
    vals = [r.get(key) for r in rows if r.get(key) is not None]
    if not vals:
        return None
    if isinstance(vals[0], bool):
        return sum(1 for v in vals if v) / len(vals)
    return None


def run_suite(
    decompiler: Path,
    corpus: Path,
    manifest: list[dict],
    limit: int | None,
    cc: str,
    label: str,
) -> dict:
    rows = []
    for item in list_samples(corpus, manifest, limit):
        name = item.get("name") or Path(item["path"]).name
        bin_path = Path(item.get("path") or corpus / name)
        if not bin_path.is_file():
            alt = corpus / name
            if alt.is_file():
                bin_path = alt
            else:
                continue
        opt = item.get("opt") or parse_opt(name)
        source_path = None
        sp = item.get("source_path")
        if sp:
            source_path = Path(sp)
        rows.append(
            run_decompiler(
                decompiler,
                bin_path,
                opt,
                corpus.parent / "artifacts" / label,
                cc,
                source_path,
                opt if opt.startswith("O") else "O2",
            )
        )
    return {
        "label": label,
        "decompiler": str(decompiler),
        "samples": rows,
        "summary": summarize(rows),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decompiler", required=True, help="path to retdec-decompiler")
    ap.add_argument("--corpus", required=True, help="directory of test binaries")
    ap.add_argument("--out", default="results/decompilebench.json")
    ap.add_argument("--baseline-decompiler", help="stock RetDec for two-column compare")
    ap.add_argument("--limit", type=int, help="max samples (CI core uses 9)")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()

    decompiler = Path(args.decompiler)
    corpus = Path(args.corpus)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(corpus)

    fork = run_suite(decompiler, corpus, manifest, args.limit, args.cc, "fork")
    payload: dict = {
        "harness": "decompilebench",
        "corpus": str(corpus),
        "cc": args.cc,
        "fork": fork,
        "samples": fork["samples"],
        "summary": fork["summary"],
    }

    if args.baseline_decompiler:
        baseline_dec = Path(args.baseline_decompiler)
        if baseline_dec.is_file():
            stock = run_suite(baseline_dec, corpus, manifest, args.limit, args.cc, "stock")
            payload["stock_retdec"] = stock
            payload["compare"] = {
                "fork_vs_stock": {
                    "recompile_success_rate": {
                        "fork": fork["summary"].get("recompile_success_rate"),
                        "stock": stock["summary"].get("recompile_success_rate"),
                    },
                    "coverage_equivalence_rate": {
                        "fork": fork["summary"].get("coverage_equivalence_rate"),
                        "stock": stock["summary"].get("coverage_equivalence_rate"),
                    },
                }
            }

    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} ({len(fork['samples'])} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
