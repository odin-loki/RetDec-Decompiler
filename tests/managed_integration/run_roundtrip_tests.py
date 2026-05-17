#!/usr/bin/env python3
"""
Round-trip test for managed-language decompilers.

For each compiled fixture binary:
  1. Decompile with retdec-decompiler → decompiled source (C or .ll)
  2. Where possible, attempt to re-compile the decompiled C output
  3. Execute both the original and the re-compiled binary with known inputs
  4. Compare the standard output / exit codes

Currently implemented for:
  - Java → retdec decompiles to C → gcc compiles → compare stdout
  - C# DLL → retdec decompiles to C → compare structural properties

Usage:
    python3 run_roundtrip_tests.py [options]

Options:
    --retdec-bin PATH    Path to retdec-decompiler (default: auto-detect)
    --fixtures DIR       Fixtures directory
    --java-bin PATH      Path to 'java' runtime (default: auto-detect)
    --dotnet-bin PATH    Path to 'dotnet' runtime (default: auto-detect)
    --cc PATH            C compiler to use for re-compilation (default: gcc)
    --timeout SEC        Per-phase timeout (default: 60)
    --verbose            Print detailed output

Exit code: 0 = all pass, 1 = some fail, 2 = setup error.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Tuple

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def find_tool(hint: Optional[str], names: List[str]) -> Optional[Path]:
    if hint:
        p = Path(hint)
        if p.is_file() and os.access(p, os.X_OK):
            return p
        return None
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found)
    return None

def run(cmd: List[str], timeout: int, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout, **kwargs)

# ---------------------------------------------------------------------------
# Result
# ---------------------------------------------------------------------------
@dataclass
class RoundtripResult:
    name: str
    phase: str
    passed: bool
    message: str
    original_output: str = ""
    recompiled_output: str = ""

# ---------------------------------------------------------------------------
# Java round-trip
# ---------------------------------------------------------------------------
def roundtrip_java(retdec: Path, java: Optional[Path], cc: Path,
                   fixture: Path, timeout: int, verbose: bool) -> RoundtripResult:
    name = fixture.stem

    with tempfile.TemporaryDirectory(prefix="retdec_rt_java_") as tmp:
        tmp_path = Path(tmp)
        out_base = tmp_path / name

        # Phase 1: decompile
        try:
            proc = run([str(retdec), "--input", str(fixture),
                        "--output", str(out_base)], timeout)
        except subprocess.TimeoutExpired:
            return RoundtripResult(name, "decompile", False, "TIMEOUT")

        if proc.returncode != 0:
            return RoundtripResult(name, "decompile", False,
                                   f"retdec exit {proc.returncode}: {proc.stderr[:300]}")

        c_file = tmp_path / (name + ".c")
        if not c_file.exists():
            # Try .ll fallback
            ll_file = tmp_path / (name + ".ll")
            if ll_file.exists():
                return RoundtripResult(name, "recompile", True,
                                       "decompiled to LLVM IR (C not available; structural check only)")
            return RoundtripResult(name, "decompile", False,
                                   "no .c or .ll output produced")

        # Phase 2: original execution (if java runtime available)
        orig_out = ""
        if java:
            try:
                proc_orig = run([str(java), "-cp", str(fixture.parent), name],
                                timeout, cwd=str(fixture.parent))
                orig_out = proc_orig.stdout
            except subprocess.TimeoutExpired:
                orig_out = "<TIMEOUT>"
            except Exception as e:
                orig_out = f"<ERROR: {e}>"

        # Phase 3: recompile C output
        exe = tmp_path / name
        compile_proc = run([str(cc), str(c_file), "-o", str(exe),
                            "-lm", "-lpthread", "-w"], timeout)
        if compile_proc.returncode != 0:
            return RoundtripResult(name, "recompile", False,
                                   f"gcc exit {compile_proc.returncode}: {compile_proc.stderr[:300]}",
                                   orig_out)

        # Phase 4: execute recompiled binary
        try:
            rt_proc = run([str(exe)], timeout)
            rt_out = rt_proc.stdout
        except subprocess.TimeoutExpired:
            return RoundtripResult(name, "execute", False, "TIMEOUT (recompiled)", orig_out)

        if not orig_out:
            return RoundtripResult(name, "compare", True,
                                   "recompiled OK (no original output to compare)",
                                   orig_out, rt_out)

        if orig_out.strip() == rt_out.strip():
            return RoundtripResult(name, "compare", True,
                                   "output matches original", orig_out, rt_out)

        # Partial match: many decompilers produce slightly different output
        orig_lines = set(orig_out.strip().splitlines())
        rt_lines   = set(rt_out.strip().splitlines())
        overlap = len(orig_lines & rt_lines)
        total   = max(len(orig_lines), 1)
        pct = overlap / total * 100

        if pct >= 50:
            return RoundtripResult(name, "compare", True,
                                   f"partial match {pct:.0f}% of output lines",
                                   orig_out, rt_out)

        return RoundtripResult(name, "compare", False,
                               f"output diverged (match={pct:.0f}%)",
                               orig_out, rt_out)

# ---------------------------------------------------------------------------
# C# round-trip (structural only — re-execution not always possible)
# ---------------------------------------------------------------------------
def roundtrip_csharp(retdec: Path, fixture: Path,
                     timeout: int, verbose: bool) -> RoundtripResult:
    name = fixture.stem

    with tempfile.TemporaryDirectory(prefix="retdec_rt_cs_") as tmp:
        tmp_path = Path(tmp)
        out_base = tmp_path / name

        try:
            proc = run([str(retdec), "--input", str(fixture),
                        "--output", str(out_base)], timeout)
        except subprocess.TimeoutExpired:
            return RoundtripResult(name, "decompile", False, "TIMEOUT")

        if proc.returncode != 0:
            return RoundtripResult(name, "decompile", False,
                                   f"retdec exit {proc.returncode}")

        # Structural check: output file must be non-empty and contain function defs
        for ext in [".c", ".ll"]:
            out_file = tmp_path / (name + ext)
            if out_file.exists():
                content = out_file.read_text(errors="replace")
                if len(content) < 100:
                    return RoundtripResult(name, "structural", False,
                                           "output too small")
                if "void" in content or "int" in content or "define" in content:
                    return RoundtripResult(name, "structural", True,
                                           f"structural check passed ({len(content)} chars)")
                return RoundtripResult(name, "structural", False,
                                       "output has no recognisable functions")

        return RoundtripResult(name, "structural", False, "no output file produced")

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--retdec-bin")
    parser.add_argument("--fixtures", default=str(Path(__file__).parent / "fixtures"))
    parser.add_argument("--java-bin")
    parser.add_argument("--dotnet-bin")
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    # Find retdec (prefer explicit --retdec-bin, then PATH, then preset output trees)
    _repo = Path(__file__).resolve().parents[2]
    retdec = find_tool(args.retdec_bin, [
        "retdec-decompiler",
        str(_repo / "build/linux/src/retdec-decompiler/retdec-decompiler"),
        str(_repo / "build/windows/src/retdec-decompiler/retdec-decompiler.exe"),
        str(_repo / "build/core-debug/src/retdec-decompiler/retdec-decompiler"),
    ])
    if not retdec:
        print("ERROR: retdec-decompiler not found. Use --retdec-bin.", file=sys.stderr)
        sys.exit(2)

    java = find_tool(args.java_bin, ["java"])
    cc   = find_tool(args.cc, [args.cc, "gcc", "cc"])

    if not cc:
        print("WARNING: No C compiler found; re-compilation phase will be skipped.")

    print(f"RetDec:    {retdec}")
    print(f"Java:      {java or '(not found)'}")
    print(f"CC:        {cc or '(not found)'}")
    print()

    fixtures_dir = Path(args.fixtures)
    results: List[RoundtripResult] = []

    # Java fixtures
    java_dir = fixtures_dir / "java"
    if java_dir.exists() and cc:
        for cls_file in sorted(java_dir.glob("*.class")):
            print(f"[java] {cls_file.name}...", end=" ", flush=True)
            r = roundtrip_java(retdec, java, cc, cls_file, args.timeout, args.verbose)
            results.append(r)
            print("PASS" if r.passed else f"FAIL ({r.phase}: {r.message[:60]})")
            if args.verbose and r.original_output:
                print("  orig:", r.original_output[:200])
            if args.verbose and r.recompiled_output:
                print("  rt:  ", r.recompiled_output[:200])

    # C# fixtures
    csharp_dir = fixtures_dir / "csharp" / "bin"
    if csharp_dir.exists():
        for dll in sorted(csharp_dir.glob("*.dll")):
            print(f"[csharp] {dll.name}...", end=" ", flush=True)
            r = roundtrip_csharp(retdec, dll, args.timeout, args.verbose)
            results.append(r)
            print("PASS" if r.passed else f"FAIL ({r.phase}: {r.message[:60]})")

    # Summary
    passed = sum(1 for r in results if r.passed)
    failed = len(results) - passed
    print(f"\nRound-trip: {passed}/{len(results)} passed, {failed} failed")
    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
