#!/usr/bin/env python3
"""CC-CONV — a calling convention must not claim a capacity it has no
registers for.

Why this exists
---------------
`arm_conv.cpp` set

    _numOfFPRegsPerParam = 2;
    _numOfVectorRegsPerParam = 4;

and declared neither `_paramFPRegs` nor `_paramDoubleRegs` nor
`_paramVectorRegs`. It was the only one of the eleven conventions that did.
AAPCS-VFP passes floating-point arguments in s0-s15 and d0-d7 and returns in
s0 or d0, and this project's ARM corpus is hard-float, so every floating-point
parameter and return value on 32-bit ARM was invisible to parameter recovery --
while the two "how many registers may one parameter span" numbers sat there
looking like the subject had been thought about.

Nothing failed. `usesFPRegistersForParameters()` returns false when both lists
are empty, so the analysis simply never looked. That is the shape this checks
for: a declaration that reads as configured and is inert.

The rule
--------
If a convention sets `_numOfFPRegsPerParam`, it must declare `_paramFPRegs` or
`_paramDoubleRegs`. If it sets `_numOfVectorRegsPerParam`, it must declare
`_paramVectorRegs`. The converse is not required: a convention may name
registers without capping how many one parameter spans.

Usage: python3 scripts/ci/check_calling_conventions.py [--self-test]
"""
from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONV_DIR = ROOT / "src" / "bin2llvmir" / "providers" / "calling_convention"

# (the "how many" knob, the register lists any one of which satisfies it)
RULES = [
    ("_numOfFPRegsPerParam", ("_paramFPRegs", "_paramDoubleRegs")),
    ("_numOfVectorRegsPerParam", ("_paramVectorRegs",)),
]


def assigns(text: str, name: str) -> bool:
    return re.search(r"\b%s\s*=" % re.escape(name), text) is not None


def check_dir(d: Path) -> list[str]:
    problems = []
    for f in sorted(d.rglob("*.cpp")):
        text = f.read_text(encoding="utf-8", errors="replace")
        for knob, lists in RULES:
            if not assigns(text, knob):
                continue
            if any(assigns(text, l) for l in lists):
                continue
            try:
                shown = f.relative_to(ROOT)
            except ValueError:
                shown = f
            problems.append(
                "%s sets %s but declares none of %s"
                % (shown, knob, " or ".join(lists))
            )
    return problems


def self_test() -> int:
    # A convention that sets the knob and names no registers must be rejected;
    # one that names them must not be. Without both halves this check could
    # pass by never looking at anything.
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        (d / "bad_conv.cpp").write_text(
            "X::X() { _numOfFPRegsPerParam = 2; }\n", encoding="utf-8"
        )
        bad = check_dir(d)
        if len(bad) != 1:
            print("CC-CONV: self-test FAIL expected 1 problem, got %d" % len(bad),
                  file=sys.stderr)
            return 1

        (d / "bad_conv.cpp").write_text(
            "X::X() { _numOfFPRegsPerParam = 2; _paramFPRegs = {1}; }\n",
            encoding="utf-8",
        )
        good = check_dir(d)
        if good:
            print("CC-CONV: self-test FAIL a convention that names registers was "
                  "rejected: %s" % good, file=sys.stderr)
            return 1

        (d / "vec_conv.cpp").write_text(
            "X::X() { _numOfVectorRegsPerParam = 4; }\n", encoding="utf-8"
        )
        if len(check_dir(d)) != 1:
            print("CC-CONV: self-test FAIL the vector rule did not fire",
                  file=sys.stderr)
            return 1
    print("CC-CONV: self-test OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv[1:]:
        return self_test()

    if not CONV_DIR.is_dir():
        print("CC-CONV: FAIL %s not found" % CONV_DIR, file=sys.stderr)
        return 1

    files = list(CONV_DIR.rglob("*.cpp"))
    if len(files) < 10:
        print("CC-CONV: FAIL expected at least 10 conventions, found %d"
              % len(files), file=sys.stderr)
        return 1

    problems = check_dir(CONV_DIR)
    if problems:
        print("CC-CONV: FAIL a convention claims a capacity it has no registers for:",
              file=sys.stderr)
        for p in problems:
            print("    %s" % p, file=sys.stderr)
        return 1

    print("CC-CONV: OK %d calling convention(s), none claims a capacity it has "
          "no registers for" % len(files))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
