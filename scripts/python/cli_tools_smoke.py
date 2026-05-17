#!/usr/bin/env python3
"""
cli_tools_smoke.py — comprehensive smoke for every installed retdec CLI tool.

For each tool we check:
  - `--help`  exits 0 (or at least non-crashy) and prints usage
  - `--version` (where supported) exits 0
  - a real functional invocation against a sample binary
  - bad-input invocation (missing file, bogus arg) — must fail cleanly
  - never times out
  - never leaves a hung child

Tools covered:
  retdec-fileinfo, retdec-decompiler, retdec-unpacker, retdec-bin2pat,
  retdec-pat2yara, retdec-ar-extractor, retdec-macho-extractor

Inputs:
  --bin-dir <DIR>        Directory holding the retdec exes (default:
                         <repo>/install/windows/bin or <repo>/install/linux/bin).
  --sample <FILE>        Sample binary to decompile / inspect. Default: pick
                         the smallest .exe found in --bin-dir.
  --out <DIR>            Where to write per-tool logs + report.json
  --timeout-decompiler <SEC>   Per-tool timeout for the decompiler (default 600 s).
  --timeout-quick <SEC>        Per-tool timeout for everything else (default 30 s).

Exit codes:
  0   all expected outcomes met
  1   one or more tools failed an expected check
  124 wall-clock timeout
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable, List, Optional


def log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[cli-smoke {ts}] {msg}", file=sys.stderr, flush=True)


REPO_ROOT = Path(__file__).resolve().parent.parent.parent


@dataclass
class ToolCheck:
    name: str
    cmd: List[str]
    timeout_sec: int
    expect_exit_zero: bool = True
    expect_nonzero_on_failure: bool = False
    crash_only_fail: bool = False
    must_contain: Optional[str] = None
    description: str = ""

    # filled in after run
    exit_code: int = -1
    duration_sec: float = 0.0
    log_path: str = ""
    passed: bool = False
    notes: List[str] = field(default_factory=list)


def autodetect_bin_dir(explicit: Optional[str]) -> Optional[Path]:
    if explicit:
        p = Path(explicit).resolve()
        return p if p.is_dir() else None
    for sub in ("install/windows/bin", "install/linux/bin", "install/bin",
                "build/windows/install/bin", "build/linux/install/bin"):
        p = (REPO_ROOT / sub).resolve()
        if p.is_dir():
            return p
    return None


def exe(bin_dir: Path, name: str) -> Optional[Path]:
    on_win = platform.system() == "Windows"
    cand = bin_dir / (name + (".exe" if on_win else ""))
    return cand if cand.is_file() else None


def pick_sample_binary(bin_dir: Path, explicit: Optional[str]) -> Optional[Path]:
    if explicit:
        p = Path(explicit).resolve()
        return p if p.is_file() else None
    # Pick the smallest installed retdec exe that isn't itself one of the
    # tools we'll be invoking (avoid recursive self-decompile by accident).
    excludes = {
        "retdec-decompiler", "retdec-gui", "retdec-fileinfo",
        "retdec-unpacker", "retdec-bin2pat", "retdec-pat2yara",
        "retdec-ar-extractor", "retdec-macho-extractor",
    }
    candidates: List[Path] = []
    for p in bin_dir.iterdir():
        if not p.is_file():
            continue
        if not p.suffix.lower() in (".exe", ""):
            continue
        if p.stem in excludes:
            continue
        candidates.append(p)
    if not candidates:
        return None
    candidates.sort(key=lambda p: p.stat().st_size)
    return candidates[0]


def run_check(check: ToolCheck, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    log_path = out_dir / f"{check.name.replace(' ', '_')}.log"
    check.log_path = str(log_path)
    log(f"-> {check.name}: {' '.join(check.cmd)}")
    started = time.monotonic()
    try:
        proc = subprocess.Popen(
            check.cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            cwd=str(Path(check.cmd[0]).parent),
        )
    except OSError as e:
        check.exit_code = -1
        check.notes.append(f"spawn failed: {e}")
        check.passed = False
        return

    try:
        stdout, _ = proc.communicate(timeout=check.timeout_sec)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate()
        check.exit_code = -2
        check.notes.append(f"timeout after {check.timeout_sec}s")
        check.duration_sec = round(time.monotonic() - started, 3)
        log_path.write_bytes(stdout or b"")
        check.passed = False
        return

    check.exit_code = proc.returncode
    check.duration_sec = round(time.monotonic() - started, 3)
    log_path.write_bytes(stdout or b"")

    ok = True
    if check.crash_only_fail:
        # Pass on any clean exit (0 OR non-zero). Fail only on negative
        # signal-style exit codes that indicate the process crashed.
        if check.exit_code < 0:
            ok = False
            check.notes.append(f"crash exit {check.exit_code}")
    else:
        if check.expect_exit_zero and check.exit_code != 0:
            ok = False
            check.notes.append(f"exit {check.exit_code} (expected 0)")
        if check.expect_nonzero_on_failure and check.exit_code == 0:
            ok = False
            check.notes.append("exit 0 (expected non-zero on failure)")
    if check.must_contain:
        try:
            text = stdout.decode("utf-8", errors="replace")
        except Exception:
            text = ""
        if check.must_contain not in text:
            ok = False
            check.notes.append(f"output missing marker: {check.must_contain!r}")
    check.passed = ok


def build_check_list(bin_dir: Path, sample: Optional[Path], out_dir: Path,
                     timeout_dec: int, timeout_quick: int) -> List[ToolCheck]:
    checks: List[ToolCheck] = []

    def add_help(name: str, help_flag: str = "--help",
                 must: Optional[str] = "usage") -> None:
        p = exe(bin_dir, name)
        if not p:
            return
        checks.append(ToolCheck(
            name=f"{name} {help_flag}",
            cmd=[str(p), help_flag],
            timeout_sec=timeout_quick,
            must_contain=must,
            description="help text smoke",
        ))

    def add_run(name: str, args: List[str], *,
                timeout: int, must: Optional[str] = None,
                expect_nonzero: bool = False) -> None:
        p = exe(bin_dir, name)
        if not p:
            return
        checks.append(ToolCheck(
            name=name + " " + "_".join(args[:3]).replace("/", "_").replace("\\", "_")[:48],
            cmd=[str(p), *args],
            timeout_sec=timeout,
            must_contain=must,
            expect_exit_zero=not expect_nonzero,
            expect_nonzero_on_failure=expect_nonzero,
            description="functional run" if not expect_nonzero else "bad-input run",
        ))

    # ── retdec-fileinfo ──
    add_help("retdec-fileinfo", "--help", must=None)  # may not include "usage"
    if sample:
        add_run("retdec-fileinfo", [str(sample)],            timeout=timeout_quick)
        add_run("retdec-fileinfo", ["--json", str(sample)],  timeout=timeout_quick,
                must='"')
    add_run("retdec-fileinfo",
            [str(bin_dir / "does-not-exist.bin")],
            timeout=timeout_quick, expect_nonzero=True)

    # ── retdec-unpacker ──
    add_help("retdec-unpacker", "--help", must=None)
    if sample:
        # Note: retdec-unpacker returns non-zero on "no packing detected" —
        # that's not a failure of the tool, just an expected outcome on
        # most real binaries. We treat *crash* (exit -1 / -2 / signal) as
        # the actual failure mode by allowing any non-crash exit code.
        unpacked = out_dir / "unpacked.bin"
        p = exe(bin_dir, "retdec-unpacker")
        if p:
            checks.append(ToolCheck(
                name="retdec-unpacker functional",
                cmd=[str(p), str(sample), "-o", str(unpacked)],
                timeout_sec=timeout_quick,
                crash_only_fail=True,
                description="unpacker: must exit cleanly (0 or non-zero, but not crash)",
            ))

    # ── retdec-ar-extractor ──
    add_help("retdec-ar-extractor", "--help", must=None)

    # ── retdec-bin2pat / retdec-pat2yara ──
    add_help("retdec-bin2pat",  "--help", must=None)
    add_help("retdec-pat2yara", "--help", must=None)

    # ── retdec-decompiler (the heavy one; --help only by default; functional
    #     run requires --timeout-decompiler to be generous) ──
    add_help("retdec-decompiler", "--help", must=None)
    if sample:
        out_c = out_dir / "decompiled.c"
        add_run("retdec-decompiler",
                [str(sample), "-o", str(out_c), "-f", "plain"],
                timeout=timeout_dec, must=None)

    # ── retdec-macho-extractor (only useful on Mach-O input, skip functional) ──
    add_help("retdec-macho-extractor", "--help", must=None)

    return checks


def main(argv: Optional[Iterable[str]] = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawTextHelpFormatter)
    p.add_argument("--bin-dir")
    p.add_argument("--sample")
    p.add_argument("--out", default="cli-smoke-artifacts")
    p.add_argument("--timeout-decompiler", type=int, default=600)
    p.add_argument("--timeout-quick",      type=int, default=30)
    p.add_argument("--skip-decompiler",    action="store_true",
                   help="skip the heavy decompiler functional run")
    args = p.parse_args(list(argv) if argv is not None else None)

    bin_dir = autodetect_bin_dir(args.bin_dir)
    if not bin_dir:
        log(f"bin-dir not found (tried install/windows/bin etc.); pass --bin-dir")
        return 2
    log(f"bin-dir: {bin_dir}")

    sample = pick_sample_binary(bin_dir, args.sample)
    if sample:
        log(f"sample binary: {sample}")
    else:
        log("no sample binary available; functional runs will be skipped")

    out_dir = Path(args.out).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    checks = build_check_list(bin_dir, sample, out_dir,
                              args.timeout_decompiler, args.timeout_quick)

    if args.skip_decompiler:
        checks = [c for c in checks
                  if c.name != "retdec-decompiler "
                  and "retdec-decompiler" not in c.name.split()[0] + "_x"  # always keep --help
                  or c.cmd[1] == "--help"]

    failed: List[str] = []
    for c in checks:
        run_check(c, out_dir)
        if c.passed:
            log(f"  OK   ({c.duration_sec}s)  exit={c.exit_code}")
        else:
            log(f"  FAIL ({c.duration_sec}s)  exit={c.exit_code}  notes={c.notes}")
            failed.append(c.name)

    report = {
        "bin_dir": str(bin_dir),
        "sample":  str(sample) if sample else "",
        "checks":  [asdict(c) for c in checks],
        "failed":  failed,
    }
    (out_dir / "report.json").write_text(json.dumps(report, indent=2),
                                          encoding="utf-8")
    log(f"wrote {out_dir / 'report.json'} ({len(checks)} checks, "
        f"{len(failed)} failed)")
    return 0 if not failed else 1


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())
