#!/usr/bin/env python3
"""IR2HLL-01 -- every LLVM instruction opcode is either converted or excluded.

Why this exists
---------------
`LLVMInstructionConverter::visitInstruction()` calls `FAIL()`, which aborts the
process. It is the fallback for any instruction the IR-to-HLL expression
converter has no case for, so an instruction that reaches it does not produce
a wrong answer -- it ends the decompilation of the whole binary.

Three of those were found in this branch, all the same way: something started
translating an instruction that used to come out as a pseudo-asm call, and the
new IR reached a converter written before that instruction existed.

    fneg        LLVM 11 replaced the `fsub -0.0, x` idiom with a real unary
                operator. Every floating-point negation, on every
                architecture, since the initial import.
    freeze      LLVM's own optimiser introduces it; no translator emits one.
    atomicrmw   BasicBlockConverter converts it as a statement and
                LLVMInstructionConverter had no case, so an atomicrmw whose
                result is used -- which is every ARM64 LSE atomic -- aborted.

Each was found by a corpus run, after the fact. This asks the question up
front, off the LLVM headers the build uses.

What it checks
--------------
An instruction reaches the expression converter unless
`LLVMSupport::isInlinableInst()` or `shouldBeConvertedAsInst()` keeps it out.
So for every opcode in LLVM's own `Instruction.def`, one of these must hold:

  * the converter defines `visit<Class>`;
  * `isInlinableInst()` names `llvm::<Class>` in its exclusion list;
  * it is a terminator, which `isInlinableInst()` excludes by
    `isTerminator()`;
  * it is in ACCOUNTED_ELSEWHERE below, with the reason.

Anything else is an abort waiting for the right binary, and this names it.

Usage:
  python3 scripts/ci/check_ir2hll_opcodes.py [--instruction-def PATH]
                                             [--self-test]
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONVERTER = "src/llvmir2hll/llvm/llvmir2bir_converter/llvm_instruction_converter.cpp"
SUPPORT = "src/llvmir2hll/llvm/llvm_support.cpp"
VALUE_CONVERTER = "src/llvmir2hll/llvm/llvmir2bir_converter/llvm_value_converter.cpp"

# Opcodes that reach neither the converter nor isInlinableInst's name list, and
# why that is safe. Each entry is a claim about the code, not a waiver: if one
# stops being true the abort comes back, so they are spelled out rather than
# skipped.
ACCOUNTED_ELSEWHERE = {
    "AllocaInst":
        "shouldBeConvertedAsInst() excludes it explicitly, above isInlinableInst",
    "StoreInst":
        "result type is void, which isInlinableInst() excludes",
    "FenceInst":
        "result type is void, which isInlinableInst() excludes",
    "LandingPadInst":
        "exception handling; no translator emits one and machine code has none",
    "CatchPadInst":
        "exception handling; no translator emits one and machine code has none",
    "CleanupPadInst":
        "exception handling; no translator emits one and machine code has none",
}


def find_instruction_def() -> Path | None:
    cfg = shutil.which("llvm-config-20") or shutil.which("llvm-config-21") \
        or shutil.which("llvm-config")
    if cfg:
        inc = subprocess.run([cfg, "--includedir"], capture_output=True,
                             text=True).stdout.strip()
        p = Path(inc) / "llvm" / "IR" / "Instruction.def"
        if p.is_file():
            return p
    for p in sorted(Path("/usr/lib").glob("llvm-*/include/llvm/IR/Instruction.def")):
        return p
    for p in sorted(ROOT.glob("build/*/external/src/llvm-project/llvm/include/llvm/IR/Instruction.def")):
        return p
    return None


def opcodes(path: Path):
    """(kind, opcode, class) for every HANDLE_*_INST line."""
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(r'HANDLE_(\w+?)_INST\s*\(\s*\d+\s*,\s*(\w+)\s*,\s*(\w+)\s*\)', line)
        if m:
            out.append((m.group(1), m.group(2), m.group(3)))
    return out


def inlinable_body(support: str) -> str:
    m = re.search(r'bool LLVMSupport::isInlinableInst.*?\n\}\n', support, re.S)
    if not m:
        raise SystemExit("IR2HLL-01: FAIL cannot find isInlinableInst() in " + SUPPORT)
    return m.group(0)


def check(root: Path, defpath: Path) -> int:
    conv = (root / CONVERTER).read_text(encoding="utf-8")
    sup = (root / SUPPORT).read_text(encoding="utf-8")
    inl = inlinable_body(sup)

    rows = opcodes(defpath)
    if len(rows) < 40:
        print(f"IR2HLL-01: FAIL only {len(rows)} opcodes parsed from {defpath};"
              " the file format changed", file=sys.stderr)
        return 2

    unaccounted = []
    counts = {"visited": 0, "not-inlinable": 0, "terminator": 0, "elsewhere": 0}
    for kind, op, cls in rows:
        if kind == "TERM":
            counts["terminator"] += 1
            continue
        if f"visit{cls}(" in conv:
            counts["visited"] += 1
            continue
        if f"llvm::{cls}>" in inl:
            counts["not-inlinable"] += 1
            continue
        if cls in ACCOUNTED_ELSEWHERE:
            counts["elsewhere"] += 1
            continue
        unaccounted.append((op, cls))

    print(f"IR2HLL-01: {len(rows)} opcodes in {defpath.parent.parent.parent}")
    print(f"IR2HLL-01:   {counts['visited']:3d} have a visit* in the converter")
    print(f"IR2HLL-01:   {counts['not-inlinable']:3d} named in isInlinableInst()'s exclusion list")
    print(f"IR2HLL-01:   {counts['terminator']:3d} terminators, excluded by isTerminator()")
    print(f"IR2HLL-01:   {counts['elsewhere']:3d} accounted for elsewhere, with a reason")

    if unaccounted:
        print("IR2HLL-01: FAIL these opcodes reach visitInstruction(), which aborts:",
              file=sys.stderr)
        for op, cls in unaccounted:
            print(f"           {op} ({cls})", file=sys.stderr)
        print("           Give the converter a visit%s, or exclude it in"
              " isInlinableInst(), or" % unaccounted[0][1], file=sys.stderr)
        print("           add it to ACCOUNTED_ELSEWHERE with the reason it"
              " cannot arrive.", file=sys.stderr)
        return 1

    print("IR2HLL-01: OK every opcode is converted or cannot reach the converter")
    return 0


def self_test(defpath: Path) -> int:
    """Neuter one accounted-for opcode at a time and require a failure.

    Without this the checker would pass on a tree where every opcode is
    unaccounted for -- as long as the parse also broke.
    """
    fails = 0
    cases = [
        (CONVERTER, "visitFreezeInst", "visitFreezeInstDISABLED",
         "an opcode whose converter case is gone"),
        (SUPPORT, "llvm::isa<llvm::AtomicRMWInst>(i) ||",
         "llvm::isa<llvm::VAArgInst>(i) ||",
         "an opcode dropped from the exclusion list"),
    ]
    for rel, old, new, name in cases:
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            for f in (CONVERTER, SUPPORT):
                dst = tmp / f
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_text((ROOT / f).read_text(encoding="utf-8"), encoding="utf-8")
            p = tmp / rel
            s = p.read_text(encoding="utf-8")
            if s.count(old) < 1:
                print(f"IR2HLL-01: self-test FAIL {name}: '{old}' not in {rel}",
                      file=sys.stderr)
                fails += 1
                continue
            p.write_text(s.replace(old, new, 1), encoding="utf-8")
            rc = check(tmp, defpath)
            if rc != 1:
                print(f"IR2HLL-01: self-test FAIL {name}: expected 1, got {rc}",
                      file=sys.stderr)
                fails += 1
            else:
                print(f"IR2HLL-01: self-test ok  {name} is caught")

    rc = check(ROOT, defpath)
    if rc != 0:
        print(f"IR2HLL-01: self-test FAIL the real tree should pass, got {rc}",
              file=sys.stderr)
        fails += 1
    else:
        print("IR2HLL-01: self-test ok  the real tree passes")

    if fails:
        return 1
    print("IR2HLL-01: self-test OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--instruction-def")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    defpath = Path(args.instruction_def) if args.instruction_def else find_instruction_def()
    if defpath is None or not defpath.is_file():
        print("IR2HLL-01: FAIL no llvm/IR/Instruction.def found; pass"
              " --instruction-def PATH", file=sys.stderr)
        return 2

    if args.self_test:
        return self_test(defpath)
    return check(ROOT, defpath)


if __name__ == "__main__":
    raise SystemExit(main())
