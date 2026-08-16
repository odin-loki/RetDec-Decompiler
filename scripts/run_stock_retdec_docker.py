#!/usr/bin/env python3
"""Run stock RetDec v5.0 (remnux/retdec) on the stand-in DecompileBench corpus."""
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

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = "remnux/retdec"
STOCK_VERSION = "v5.0"
STOCK_COMMIT = "53e55b4b26e9b843787f0e06d867441e32b1604e"

INNER_SCRIPT = r"""
set -u
: > /out/timings.txt
for f in /corpus/*; do
  [ -f "$f" ] || continue
  base=$(basename "$f")
  echo "==> $base"
  start=$(date +%s.%N)
  if retdec-decompiler "$f" --output "/out/${base}.c"; then
    status=ok
  else
    status=fail
  fi
  end=$(date +%s.%N)
  echo "$base $status $start $end" >> /out/timings.txt
done
"""


def find_docker() -> str:
    env = os.environ.get("DOCKER")
    candidates = [
        env,
        r"C:\Program Files\Docker\Docker\resources\bin\docker.exe",
        shutil.which("docker") if os.name == "nt" else None,
        "/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe",
        shutil.which("docker"),
    ]
    for cand in candidates:
        if not cand:
            continue
        path = Path(cand)
        if not path.is_file():
            continue
        # WSL cannot exec docker.exe (Exec format error) unless interop is on.
        if os.name != "nt" and path.suffix.lower() == ".exe":
            continue
        probe = subprocess.run(
            [str(path), "version", "--format", "{{.Server.Version}}"],
            capture_output=True,
            text=True,
        )
        if probe.returncode == 0 and (probe.stdout or "").strip():
            return str(path)
    raise SystemExit(
        "docker daemon not reachable from this shell. "
        "On Windows (no reboot): $env:PATH = 'C:\\Program Files\\Docker\\Docker\\resources\\bin;' + $env:PATH"
    )


def to_win_volume(path: Path) -> str:
    raw = str(path.resolve())
    m = re.match(r"^/mnt/([a-zA-Z])/(.*)$", raw.replace("\\", "/"))
    if m:
        return f"{m.group(1).upper()}:\\{m.group(2).replace('/', '\\')}"
    return str(path.resolve())


def docker_cmd(docker: str, *args: str) -> list[str]:
    return [docker, *args]


def parse_opt(name: str) -> str:
    m = re.search(r"-(O\d)$", name, re.IGNORECASE)
    return m.group(1).upper() if m else "unknown"


def to_wsl_path(path: Path) -> str:
    raw = str(path.resolve())
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", raw)
    if m:
        return f"/mnt/{m.group(1).lower()}/{m.group(2).replace(chr(92), '/')}"
    return raw.replace("\\", "/")


def try_compile(source_c: Path, cc: str) -> bool:
    extra = []
    text = source_c.read_text(encoding="utf-8", errors="replace")
    if "pthread" in text:
        extra.append("-pthread")
    if shutil.which(cc):
        with tempfile.TemporaryDirectory() as td:
            exe = Path(td) / "a.out"
            proc = subprocess.run(
                [cc, "-O0", "-std=c11", "-o", str(exe), str(source_c), *extra],
                capture_output=True,
                text=True,
            )
            return proc.returncode == 0
    if os.name == "nt":
        wsl_c = to_wsl_path(source_c)
        flags = " ".join(extra)
        proc = subprocess.run(
            [
                "wsl",
                "-d",
                "Ubuntu",
                "-e",
                "bash",
                "-lc",
                f"{cc} -O0 -std=c11 -o /tmp/retdec-stock-a.out '{wsl_c}' {flags}",
            ],
            capture_output=True,
            text=True,
        )
        return proc.returncode == 0
    return False


def rate(rows: list[dict], key: str) -> float | None:
    vals = [r.get(key) for r in rows if r.get(key) is not None]
    if not vals:
        return None
    return sum(1 for v in vals if v) / len(vals)


def summarize(rows: list[dict]) -> dict:
    by_opt: dict[str, list[dict]] = {}
    for row in rows:
        by_opt.setdefault(row.get("opt", "unknown"), []).append(row)
    per_opt = {
        opt: {
            "count": len(group),
            "syntax_valid_rate": rate(group, "syntax_valid"),
            "recompile_success_rate": rate(group, "recompile_success"),
            "coverage_equivalence_rate": rate(group, "coverage_equivalence"),
            "mean_wall_s": round(sum(r.get("wall_s", 0.0) for r in group) / len(group), 3) if group else 0.0,
        }
        for opt, group in sorted(by_opt.items())
    }
    return {
        "count": len(rows),
        "syntax_valid": sum(1 for r in rows if r.get("syntax_valid")),
        "recompile_ok": sum(1 for r in rows if r.get("recompile_success") is True),
        "coverage_ok": sum(1 for r in rows if r.get("coverage_equivalence") is True),
        "syntax_valid_rate": rate(rows, "syntax_valid"),
        "recompile_success_rate": rate(rows, "recompile_success"),
        "coverage_equivalence_rate": rate(rows, "coverage_equivalence"),
        "mean_wall_s": round(sum(r.get("wall_s", 0.0) for r in rows) / len(rows), 3) if rows else 0.0,
        "per_opt": per_opt,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--profile", choices=("ci-core", "full"), default="ci-core")
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ap.add_argument("--skip-pull", action="store_true")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()

    docker = find_docker()
    work = ROOT / "build" / "stock-docker-work"
    out_json = ROOT / "results" / f"stock-retdec-docker-{args.profile}.json"

    print(f"==> docker={docker}")
    if not args.skip_pull:
        print(f"==> pull {args.image}")
        subprocess.run(docker_cmd(docker, "pull", args.image), check=False)

    fetch_cmd = ["bash", str(ROOT / "scripts/fetch_decompilebench_corpus.sh"), "--profile", args.profile]
    stage_cmd = [sys.executable, str(ROOT / "scripts/_stage_stock_docker_corpus.py")]
    if os.name == "nt":
        wsl_root = to_wsl_path(ROOT)
        fetch_cmd = [
            "wsl", "-d", "Ubuntu", "-e", "bash", "-lc",
            f"cd '{wsl_root}' && bash scripts/fetch_decompilebench_corpus.sh --profile {args.profile}",
        ]
        stage_cmd = [
            "wsl", "-d", "Ubuntu", "-e", "bash", "-lc",
            f"cd '{wsl_root}' && python3 scripts/_stage_stock_docker_corpus.py",
        ]
    fetch = subprocess.run(fetch_cmd)
    if fetch.returncode != 0:
        return fetch.returncode
    stage = subprocess.run(stage_cmd)
    if stage.returncode != 0:
        return stage.returncode

    (work / "out").mkdir(parents=True, exist_ok=True)
    for stale in (work / "out").glob("*"):
        if stale.is_file():
            stale.unlink()

    corpus_vol = to_win_volume(work / "corpus")
    out_vol = to_win_volume(work / "out")
    print(f"==> stock decompile profile={args.profile} image={args.image}")
    t0 = time.time()
    proc = subprocess.run(
        docker_cmd(
            docker,
            "run",
            "--rm",
            "-u",
            "root",
            "-v",
            f"{corpus_vol}:/corpus:ro",
            "-v",
            f"{out_vol}:/out",
            args.image,
            "bash",
            "-lc",
            INNER_SCRIPT,
        )
    )
    elapsed = time.time() - t0
    print(f"==> container wall {elapsed:.1f}s rc={proc.returncode}")

    timings: dict[str, dict] = {}
    tfile = work / "out" / "timings.txt"
    if tfile.is_file():
        for line in tfile.read_text(encoding="utf-8", errors="replace").splitlines():
            parts = line.split()
            if len(parts) < 4:
                continue
            name, status, start_s, end_s = parts[0], parts[1], parts[2], parts[3]
            try:
                wall = float(end_s) - float(start_s)
            except ValueError:
                wall = 0.0
            timings[name] = {"name": name, "status": status, "wall_s": wall}

    rows = []
    for sample in sorted((work / "corpus").iterdir()):
        if not sample.is_file():
            continue
        c_out = work / "out" / f"{sample.name}.c"
        syntax = c_out.is_file() and c_out.stat().st_size > 0
        recompile = try_compile(c_out, args.cc) if syntax else False
        wall = float(timings.get(sample.name, {}).get("wall_s") or 0.0)
        rows.append(
            {
                "input": str(sample),
                "opt": parse_opt(sample.name),
                "syntax_valid": syntax,
                "recompile_success": recompile,
                "coverage_equivalence": False if syntax else None,
                "output_c": str(c_out) if c_out.is_file() else None,
                "bytes": c_out.stat().st_size if c_out.is_file() else 0,
                "wall_s": round(wall, 3),
                "stock_status": timings.get(sample.name, {}).get("status"),
            }
        )

    payload = {
        "harness": "stock_retdec_docker",
        "image": args.image,
        "stock_version": STOCK_VERSION,
        "stock_commit": STOCK_COMMIT,
        "profile": args.profile,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "docker": docker,
        "container_wall_s": round(elapsed, 3),
        "samples": rows,
        "summary": summarize(rows),
        "note": "Stock RetDec has no algorithm-label export; F1 is fork-only. coverage_equivalence is False unless recompile succeeds.",
    }
    out_json.parent.mkdir(parents=True, exist_ok=True)
    out_json.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {out_json}")
    print(json.dumps(payload["summary"], indent=2))
    return 0 if rows and payload["summary"]["syntax_valid_rate"] is not None else 2


if __name__ == "__main__":
    raise SystemExit(main())
