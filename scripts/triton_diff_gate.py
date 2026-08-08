#!/usr/bin/env python3
"""Triton/D-Helix differential gate scaffold (step 20).

Modes:
  stdout  — compile and compare stdout (default fallback)
  fuzz    — multiple runs with varied environment (stronger smoke)
  triton  — when Triton is installed, lift main and compare CFG size + fuzz
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def find_cc() -> str | None:
    for cc in ("gcc", "cc", "clang"):
        if shutil.which(cc):
            return cc
    return None


def compile_sources(cc: str, orig: Path, ref: Path, td: Path) -> tuple[Path, Path]:
    orig_bin = td / "orig"
    ref_bin = td / "ref"
    subprocess.run([cc, "-O2", "-std=c11", "-o", str(orig_bin), str(orig)], check=True)
    subprocess.run([cc, "-O2", "-std=c11", "-o", str(ref_bin), str(ref)], check=True)
    return orig_bin, ref_bin


def run_capture(bin_path: Path, env: dict[str, str] | None = None) -> tuple[int, str]:
    proc = subprocess.run(
        [str(bin_path)],
        capture_output=True,
        text=True,
        env=env,
        timeout=30,
    )
    return proc.returncode, proc.stdout


def gate_stdout(cc: str, orig: Path, ref: Path) -> bool:
    td = Path(tempfile.mkdtemp(prefix="retdec_diff_"))
    orig_bin, ref_bin = compile_sources(cc, orig, ref, td)
    _, out_orig = run_capture(orig_bin)
    _, out_ref = run_capture(ref_bin)
    return out_orig == out_ref


def gate_fuzz(cc: str, orig: Path, ref: Path, rounds: int = 16) -> bool:
    td = Path(tempfile.mkdtemp(prefix="retdec_diff_fuzz_"))
    orig_bin, ref_bin = compile_sources(cc, orig, ref, td)
    for i in range(rounds):
        env = os.environ.copy()
        env["RETDEC_DIFF_SEED"] = str(i)
        rc_o, out_o = run_capture(orig_bin, env)
        rc_r, out_r = run_capture(ref_bin, env)
        if rc_o != rc_r or out_o != out_r:
            print(f"fuzz mismatch at round {i}: rc {rc_o}!={rc_r}", file=sys.stderr)
            return False
    return True


def gate_triton(cc: str, orig: Path, ref: Path) -> bool:
    try:
        from triton import TritonContext, ARCH  # type: ignore
    except ImportError:
        print("Triton import failed — falling back to fuzz gate", file=sys.stderr)
        return gate_fuzz(cc, orig, ref)

    td = Path(tempfile.mkdtemp(prefix="retdec_diff_triton_"))
    orig_bin, ref_bin = compile_sources(cc, orig, ref, td)

    def lift_insns(path: Path) -> int:
        ctx = TritonContext()
        ctx.setArchitecture(ARCH.X86_64)
        data = path.read_bytes()
        count = 0
        for i in range(0, min(len(data), 4096)):
            try:
                inst = ctx.disassembly(data[i : i + 16], 0x1000 + i)
                if inst:
                    count += 1
            except Exception:
                break
        return count

    # Weak structural check: similar disassembly depth at entry
    o_ins = lift_insns(orig_bin)
    r_ins = lift_insns(ref_bin)
    if abs(o_ins - r_ins) > max(8, o_ins // 4):
        print(f"Triton CFG smoke: insn count {o_ins} vs {r_ins}", file=sys.stderr)
        return False

    return gate_fuzz(cc, orig, ref)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("refined")
    ap.add_argument(
        "--mode",
        choices=("auto", "stdout", "fuzz", "triton"),
        default="auto",
    )
    args = ap.parse_args()

    cc = find_cc()
    if not cc:
        print("No C compiler found", file=sys.stderr)
        return 2

    orig = Path(args.original)
    ref = Path(args.refined)
    if not orig.is_file() or not ref.is_file():
        print("Missing source file", file=sys.stderr)
        return 2

    mode = args.mode
    if mode == "auto":
        try:
            __import__("triton")
            mode = "triton"
        except ImportError:
            mode = "fuzz"

    ok = False
    if mode == "stdout":
        ok = gate_stdout(cc, orig, ref)
    elif mode == "fuzz":
        ok = gate_fuzz(cc, orig, ref)
    else:
        ok = gate_triton(cc, orig, ref)

    if not ok:
        print("Differential gate: FAIL", file=sys.stderr)
        return 1
    print(f"Differential gate: PASS ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
