#!/usr/bin/env python3
"""DecompileBench runner (arXiv 2505.11340).

Local stand-in corpus: tests/decompilebench/corpus/ (from fetch_decompilebench_corpus.sh).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

try:
    import resource
except ImportError:
    resource = None  # type: ignore[assignment]


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _git_output(repo: Path, *args: str) -> str | None:
    try:
        proc = subprocess.run(
            ["git", *args],
            cwd=repo,
            capture_output=True,
            text=True,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return proc.stdout


def collect_provenance(repo: Path | None = None) -> dict:
    root = repo or _repo_root()
    sha_out = _git_output(root, "rev-parse", "HEAD")
    status_out = _git_output(root, "status", "--porcelain")
    cc = os.environ.get("CC", "gcc")
    cc_ver = None
    try:
        proc = subprocess.run([cc, "--version"], capture_output=True, text=True, timeout=10)
        if proc.returncode == 0 and proc.stdout:
            cc_ver = proc.stdout.splitlines()[0].strip()
    except (OSError, subprocess.TimeoutExpired):
        cc_ver = None
    uname = None
    try:
        u = subprocess.run(["uname", "-a"], capture_output=True, text=True, timeout=10)
        if u.returncode == 0:
            uname = (u.stdout or "").strip()
    except (OSError, subprocess.TimeoutExpired):
        uname = None
    nproc = os.cpu_count()
    return {
        "git_sha": sha_out.strip() if sha_out and sha_out.strip() else "unknown",
        "dirty": bool(status_out and status_out.strip()),
        "harness": "decompilebench",
        "harness_version": "1",
        "cc": cc,
        "cc_version": cc_ver,
        "uname": uname,
        "cpu_count": nproc,
        "os_name": os.name,
    }


def repo_relative_path(path: str | Path, repo: Path | None = None) -> str:
    """Store repo-relative paths when *path* resolves under the repo."""
    root = (repo or _repo_root()).resolve()
    raw = str(path)
    try:
        resolved = Path(path).resolve()
        if resolved.is_relative_to(root):
            return resolved.relative_to(root).as_posix()
    except (OSError, ValueError):
        pass
    return raw


def _relativize_obj(obj, repo: Path) -> None:
    if isinstance(obj, dict):
        for key, val in obj.items():
            if isinstance(val, str) and ("/" in val or "\\" in val):
                obj[key] = repo_relative_path(val, repo)
            else:
                _relativize_obj(val, repo)
    elif isinstance(obj, list):
        for item in obj:
            _relativize_obj(item, repo)


def _relativize_sample_paths(payload: dict, repo: Path) -> None:
    _relativize_obj(payload, repo)


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


def try_compile(
    source_c: Path,
    cc: str,
    opt: str = "O2",
    extra: list[str] | None = None,
    extra_sources: list[Path] | None = None,
) -> tuple[Path | None, str]:
    if not shutil.which(cc):
        return None, "cc missing"
    with tempfile.TemporaryDirectory() as td:
        exe = Path(td) / "a.out"
        cmd = [cc, f"-{opt}", "-std=gnu11", "-w", "-o", str(exe), str(source_c)]
        if extra_sources:
            cmd.extend(str(p) for p in extra_sources)
        if extra:
            cmd.extend(extra)
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return None, (proc.stderr or proc.stdout or "")[-400:]
        return exe, ""


def try_syntax_only(source_c: Path, cc: str) -> bool:
    if not shutil.which(cc):
        return False
    proc = subprocess.run(
        [cc, "-fsyntax-only", "-std=gnu11", "-w", str(source_c)],
        capture_output=True,
        text=True,
    )
    return proc.returncode == 0


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
    if sys.platform != "linux" or resource is None:
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


def _decompiler_env(extra_env: dict[str, str] | None, *, is_stock: bool) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("RETDEC_PROFILE_JSON", "auto")
    if is_stock:
        env.pop("RETDEC_EMIT_BUILDABLE", None)
    elif extra_env:
        env.update(extra_env)
    return env


def _source_extra_flags(source_path: Path | None) -> list[str]:
    if source_path and source_path.is_file():
        return compile_flags_for_source(
            source_path.read_text(encoding="utf-8", errors="replace")
        )
    return []


def _score_sidecar(path: Path, cc: str, compile_opt: str, extra: list[str]) -> tuple[bool | None, bool | None]:
    if not path.is_file():
        return None, None
    tu_valid = try_syntax_only(path, cc)
    dec_exe, _ = try_compile(path, cc, compile_opt, extra)
    if dec_exe is None:
        stem = path.name
        if stem.endswith(".buildable.c"):
            stubs = path.with_name(path.name[: -len(".buildable.c")] + "_stubs.c")
            if stubs.is_file():
                dec_exe, _ = try_compile(path, cc, compile_opt, extra, extra_sources=[stubs])
    return tu_valid, dec_exe is not None


def _first_existing(*candidates: Path) -> Path | None:
    for path in candidates:
        if path.is_file():
            return path
    return None


def _sidecar_candidates(output_c: Path, kind: str) -> tuple[Path, ...]:
    """Accept both `<stem>.buildable.c` and `<out.c>.buildable.c` spellings."""
    stem = output_c.with_suffix("")
    return (
        Path(str(stem) + f".{kind}.c"),
        Path(str(output_c) + f".{kind}.c"),
        output_c.with_name(output_c.name + f".{kind}.c"),
    )


def run_decompiler(
    decompiler: Path,
    input_bin: Path,
    opt: str,
    out_dir: Path,
    cc: str,
    source_path: Path | None,
    compile_opt: str,
    extra_env: dict[str, str] | None = None,
    is_stock: bool = False,
) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    out_c = out_dir / f"{input_bin.name}-{opt}.c"
    profile_path = Path(str(out_c) + ".profile.json")
    env = _decompiler_env(extra_env, is_stock=is_stock)
    t0 = time.time()
    proc = subprocess.run(
        [str(decompiler), str(input_bin), "--output", str(out_c)],
        capture_output=True,
        text=True,
        timeout=600,
        env=env,
    )
    elapsed = time.time() - t0
    rss = peak_rss_kb()
    syntax_valid = out_c.is_file() and out_c.stat().st_size > 0
    profile = None
    if profile_path.is_file():
        try:
            profile = json.loads(profile_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            profile = None
    extra = _source_extra_flags(source_path)
    tu_valid = False
    recompile = None
    cov = None
    if syntax_valid:
        tu_valid = try_syntax_only(out_c, cc)
        dec_exe, _ = try_compile(out_c, cc, compile_opt, extra)
        recompile = dec_exe is not None
        if recompile:
            cov = coverage_equivalence(input_bin, source_path, out_c, cc, compile_opt)
    output_c = str(out_c)
    buildable = _first_existing(*_sidecar_candidates(out_c, "buildable"))
    refined = _first_existing(*_sidecar_candidates(out_c, "refined"))
    tu_valid_buildable, recompile_buildable = (
        _score_sidecar(buildable, cc, compile_opt, extra) if buildable else (None, None)
    )
    tu_valid_refined, recompile_refined = (
        _score_sidecar(refined, cc, compile_opt, extra) if refined else (None, None)
    )
    row = {
        "input": str(input_bin),
        "opt": opt,
        "exit_code": proc.returncode,
        "wall_s": round(elapsed, 3),
        "peak_rss_kb": rss,
        "output_c": output_c,
        "syntax_valid": syntax_valid,
        "tu_valid": tu_valid,
        "recompile_success": recompile,
        "coverage_equivalence": cov,
        "tu_valid_buildable": tu_valid_buildable,
        "recompile_buildable": recompile_buildable,
        "tu_valid_refined": tu_valid_refined,
        "recompile_refined": recompile_refined,
        "stderr_tail": (proc.stderr or "")[-500:],
    }
    if profile is not None:
        row["profile"] = profile
        stages = {s.get("name"): s for s in profile.get("stages", []) if isinstance(s, dict)}
        neural = stages.get("analysis.neural_refine")
        if neural and neural.get("total_ms") is not None:
            row["neural_refine_wall_s"] = round(float(neural["total_ms"]) / 1000.0, 3)
    return row


def wall_stats(rows: list[dict]) -> dict:
    walls = sorted(float(r.get("wall_s", 0.0)) for r in rows)
    if not walls:
        return {
            "mean_wall_s": 0.0,
            "p50_wall_s": 0.0,
            "p90_wall_s": 0.0,
            "p99_wall_s": 0.0,
            "max_wall_s": 0.0,
        }
    n = len(walls)

    def pct(p: int) -> float:
        return round(walls[int((n - 1) * p / 100)], 3)

    return {
        "mean_wall_s": round(sum(walls) / n, 3),
        "p50_wall_s": pct(50),
        "p90_wall_s": pct(90),
        "p99_wall_s": pct(99),
        "max_wall_s": round(walls[-1], 3),
    }


def summarize(rows: list[dict]) -> dict:
    by_opt: dict[str, list[dict]] = {}
    for row in rows:
        by_opt.setdefault(row.get("opt", "unknown"), []).append(row)

    per_opt = {}
    for opt, group in sorted(by_opt.items()):
        per_opt[opt] = {
            "count": len(group),
            "syntax_valid_rate": rate_from(group, "syntax_valid"),
            "tu_valid_rate": rate_from(group, "tu_valid"),
            "recompile_success_rate": rate_from(group, "recompile_success"),
            "coverage_equivalence_rate": rate_from(group, "coverage_equivalence"),
            **wall_stats(group),
        }

    return {
        "count": len(rows),
        "syntax_valid": sum(1 for r in rows if r.get("syntax_valid")),
        "recompile_ok": sum(1 for r in rows if r.get("recompile_success") is True),
        "coverage_ok": sum(1 for r in rows if r.get("coverage_equivalence") is True),
        "syntax_valid_rate": rate_from(rows, "syntax_valid"),
        "tu_valid_rate": rate_from(rows, "tu_valid"),
        "recompile_success_rate": rate_from(rows, "recompile_success"),
        "coverage_equivalence_rate": rate_from(rows, "coverage_equivalence"),
        "tu_valid_buildable_rate": rate_from(rows, "tu_valid_buildable"),
        "recompile_buildable_rate": rate_from(rows, "recompile_buildable"),
        "tu_valid_refined_rate": rate_from(rows, "tu_valid_refined"),
        "recompile_refined_rate": rate_from(rows, "recompile_refined"),
        "buildable_count": sum(1 for r in rows if r.get("tu_valid_buildable") is not None),
        "refined_count": sum(1 for r in rows if r.get("tu_valid_refined") is not None),
        "per_opt": per_opt,
        **wall_stats(rows),
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
    extra_env: dict[str, str] | None = None,
    is_stock: bool = False,
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
                extra_env=extra_env,
                is_stock=is_stock,
            )
        )
    return {
        "label": label,
        "decompiler": str(decompiler),
        "samples": rows,
        "summary": summarize(rows),
    }


def _pair(fork_summary: dict, stock_summary: dict, key: str) -> dict:
    return {
        "fork": fork_summary.get(key),
        "stock": stock_summary.get(key),
    }


def _wall_ratio(fork_val: object, stock_val: object) -> float | None:
    if fork_val is None or stock_val in (None, 0, 0.0):
        return None
    return round(float(fork_val) / float(stock_val), 3)


def compare_fork_vs_stock(fork_summary: dict, stock_summary: dict) -> dict:
    mean_fork = fork_summary.get("mean_wall_s")
    mean_stock = stock_summary.get("mean_wall_s")
    return {
        "mean_wall_s": {
            "fork": mean_fork,
            "stock": mean_stock,
            "ratio": _wall_ratio(mean_fork, mean_stock),
        },
        "p99_wall_s": _pair(fork_summary, stock_summary, "p99_wall_s"),
        "syntax_valid_rate": _pair(fork_summary, stock_summary, "syntax_valid_rate"),
        "tu_valid_rate": _pair(fork_summary, stock_summary, "tu_valid_rate"),
        "recompile_success_rate": _pair(fork_summary, stock_summary, "recompile_success_rate"),
        "coverage_equivalence_rate": _pair(fork_summary, stock_summary, "coverage_equivalence_rate"),
    }


def _fmt_cell(value: object) -> str:
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.3f}"
    return str(value)


def write_compare_markdown(payload: dict, dest: Path) -> None:
    fork_summary = payload.get("summary") or payload.get("fork", {}).get("summary") or {}
    stock_summary = payload.get("stock_retdec", {}).get("summary") or {}
    dest.parent.mkdir(parents=True, exist_ok=True)
    rows = (
        ("count", "count", "count", "buildable_count", "refined_count"),
        ("syntax_valid_rate", "syntax_valid_rate", "syntax_valid_rate", None, None),
        ("tu_valid_rate", "tu_valid_rate", "tu_valid_rate", "tu_valid_buildable_rate", "tu_valid_refined_rate"),
        (
            "recompile_success_rate",
            "recompile_success_rate",
            "recompile_success_rate",
            "recompile_buildable_rate",
            "recompile_refined_rate",
        ),
        ("mean_wall_s", "mean_wall_s", "mean_wall_s", None, None),
        ("p50_wall_s", "p50_wall_s", "p50_wall_s", None, None),
        ("p90_wall_s", "p90_wall_s", "p90_wall_s", None, None),
        ("p99_wall_s", "p99_wall_s", "p99_wall_s", None, None),
        ("max_wall_s", "max_wall_s", "max_wall_s", None, None),
    )
    lines = [
        "# DecompileBench fork vs stock",
        "",
        "| Metric | Fork | Stock | Fork-buildable | Fork-refined |",
        "|--------|------|-------|----------------|--------------|",
    ]
    for label, fork_key, stock_key, buildable_key, refined_key in rows:
        lines.append(
            "| {metric} | {fork} | {stock} | {buildable} | {refined} |".format(
                metric=label,
                fork=_fmt_cell(fork_summary.get(fork_key) if fork_key else None),
                stock=_fmt_cell(stock_summary.get(stock_key) if stock_key else None),
                buildable=_fmt_cell(fork_summary.get(buildable_key) if buildable_key else None),
                refined=_fmt_cell(fork_summary.get(refined_key) if refined_key else None),
            )
        )
    compare = payload.get("compare", {}).get("fork_vs_stock", {})
    ratio = compare.get("mean_wall_s", {}).get("ratio")
    lines.extend(
        [
            "",
            "Wall times are decompiler process time. Sidecar columns score "
            "`.buildable.c` / `.refined.c` when present; they are not separate decompile runs.",
        ]
    )
    if ratio is not None:
        lines.append(f"fork/stock mean_wall_s ratio: {_fmt_cell(ratio)}")
    lines.append("")
    dest.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--decompiler", required=True, help="path to retdec-decompiler")
    ap.add_argument("--corpus", required=True, help="directory of test binaries")
    ap.add_argument("--out", default="results/decompilebench.json")
    ap.add_argument("--baseline-decompiler", help="stock RetDec for two-column compare")
    ap.add_argument(
        "--stock-json",
        help="attach a previous stock RetDec result JSON (docker or runner) without re-running stock",
    )
    ap.add_argument("--limit", type=int, help="max samples (CI core uses 9)")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument(
        "--emit-buildable-env",
        action="store_true",
        help="set RETDEC_EMIT_BUILDABLE=1 for the fork suite only",
    )
    ap.add_argument("--markdown-out", help="write fork vs stock vs sidecar compare table")
    args = ap.parse_args()

    decompiler = Path(args.decompiler)
    corpus = Path(args.corpus)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    manifest = load_manifest(corpus)

    fork_env = {"RETDEC_EMIT_BUILDABLE": "1"} if args.emit_buildable_env else None
    fork = run_suite(
        decompiler,
        corpus,
        manifest,
        args.limit,
        args.cc,
        "fork",
        extra_env=fork_env,
        is_stock=False,
    )
    payload: dict = {
        "harness": "decompilebench",
        "corpus": str(corpus),
        "cc": args.cc,
        "fork": fork,
        "samples": fork["samples"],
        "summary": fork["summary"],
    }

    stock = None
    if args.baseline_decompiler:
        baseline_dec = Path(args.baseline_decompiler)
        if baseline_dec.is_file():
            stock = run_suite(
                baseline_dec,
                corpus,
                manifest,
                args.limit,
                args.cc,
                "stock",
                extra_env=None,
                is_stock=True,
            )
    elif args.stock_json:
        stock_path = Path(args.stock_json)
        if stock_path.is_file():
            prior = json.loads(stock_path.read_text(encoding="utf-8"))
            prior_samples = prior.get("samples") or prior.get("stock_retdec", {}).get("samples") or []
            prior_summary = prior.get("summary") or prior.get("stock_retdec", {}).get("summary") or {}
            if prior_samples and "p50_wall_s" not in prior_summary:
                prior_summary = {**prior_summary, **summarize(prior_samples)}
            stock = {
                "label": "stock",
                "decompiler": prior.get("image") or prior.get("decompiler") or str(stock_path),
                "source_json": str(stock_path),
                "samples": prior_samples,
                "summary": prior_summary,
            }

    if stock is not None:
        payload["stock_retdec"] = stock
        payload["compare"] = {
            "fork_vs_stock": compare_fork_vs_stock(fork["summary"], stock["summary"])
        }

    repo = _repo_root()
    payload["provenance"] = collect_provenance(repo)
    _relativize_sample_paths(payload, repo)

    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} ({len(fork['samples'])} rows)")
    if args.markdown_out:
        md_path = Path(args.markdown_out)
        write_compare_markdown(payload, md_path)
        print(f"Wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
