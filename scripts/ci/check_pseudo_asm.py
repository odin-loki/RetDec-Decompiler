#!/usr/bin/env python3
"""PSEUDO-01 — of the instructions a binary contains, how many does
src/capstone2llvmir actually translate, as opposed to handing to a
pseudo-assembly call?

Why this is not COV-01
----------------------
COV-01 asks whether the dispatch table has a function pointer for an
instruction id. That is a fair proxy and it is an upper bound, because a
function pointer is not the same as a translation. On ARM64 several
translators open with

    if (ifVectorGeneratePseudo(i, ai, irb)) { return; }

and, for a vector operand, emit exactly the `__asm_movi` call a `nullptr`
entry would have emitted. COV-01 scores those covered. The decompiler's
output does not know the difference between them and a null entry, and the
output is what a user reads.

This asks the translator rather than the table. The probe
(scripts/ci/pseudo_asm_probe.cpp) translates each instruction into a throwaway
function and asks `isPseudoAsmFunctionCall()` -- the translator's own
predicate -- whether anything it produced was a pseudo-assembly call.

The two numbers are meant to be read together: COV-01 is "has a function for",
PSEUDO-01 is "translates". Where they disagree, COV-01 is the optimistic one.

Usage:
  python3 scripts/ci/check_pseudo_asm.py --corpus DIR --probe PATH
      [--arch LIST] [--top N] [--arch-min "arm64=0.99 x86_64=0.99"]
      [--min-rate F] [--self-test]
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The region logic -- which bytes of a binary are code, which are data by
# mapping symbol, and which are zero padding -- lives in COV-01 and is imported
# rather than copied. Two copies of "what counts as code" is how two gates come
# to disagree about the same binary.
_spec = importlib.util.spec_from_file_location(
    "cov01", ROOT / "scripts/ci/check_instruction_coverage.py")
cov01 = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cov01)

ARCHES = ["x86_64", "arm", "arm64", "mips", "powerpc"]


MODELLED_RE = (r'bool isAsmIntrinsicName\(const std::string& name\)\s*\{(.*?)\}')


def modelled_names() -> set[str]:
    """The `__asm_*` names the product itself declares to be intended models
    rather than unimplemented semantics.

    Read out of src/retdec/semantic_recovery_export.cpp rather than listed
    here, for the reason IR2HLL-01 reads InstVisitor.h rather than hard-coding
    its delegation chain: a gate that keeps its own copy of the product's list
    is a gate that will eventually be excusing something the product no longer
    excuses. If the function cannot be found the set is empty, which counts
    everything and errs toward reporting too much.
    """
    import re
    src = ROOT / "src/retdec/semantic_recovery_export.cpp"
    if not src.is_file():
        return set()
    m = re.search(MODELLED_RE, src.read_text(encoding="utf-8"), re.S)
    if not m:
        return set()
    return set(re.findall(r'name != "(__asm_\w+)"', m.group(1)))


def measure(corpus: Path, probe: Path, arch: str):
    """Returns (translated, pseudo-by-name Counter keyed on (fn, mnemonic))."""
    total = 0
    kinds: Counter = Counter()
    for b in sorted(corpus.glob(f"*-{arch}-gcc-*")):
        if not b.is_file():
            continue
        sec = cov01.text_section(b)
        if sec is None:
            continue
        ndx, vaddr, off, size = sec
        regions = [(off, size, arch)]
        r, _data = cov01.mapping_regions(b, arch, ndx, vaddr, off, size)
        if r:
            regions = r
        blob = b.read_bytes()
        for roff, rsize, rmode in regions:
            subs, _z = cov01.split_on_zero_runs(blob, roff, rsize)
            for soff, ssize in subs:
                out = subprocess.run(
                    [str(probe), str(b), rmode, f"{soff}:{ssize}"],
                    capture_output=True, text=True).stdout
                for line in out.splitlines():
                    p = line.split(None, 2)
                    if len(p) < 3:
                        continue
                    total += 1
                    if p[1] != "-":
                        kinds[(p[1], p[2])] += 1
    return total, kinds


def self_test(probe: Path) -> int:
    """The probe must answer 0 for an instruction the translator implements and
    1 for one it does not, on the same architecture in the same run. Without
    both halves a probe that always answered 0 -- which is what a broken
    isPseudoAsmFunctionCall() would look like -- would report a perfect rate
    and look like good news."""
    import tempfile

    work = Path(tempfile.mkdtemp(prefix="pseudo01-self-"))
    # x86-64: `nop` (translated), then `cpuid` (pseudo-asm: it writes four
    # registers from state this translator does not model).
    blob = bytes([0x90, 0x0f, 0xa2])
    f = work / "bytes.bin"
    f.write_bytes(blob)
    out = subprocess.run([str(probe), str(f), "x86_64", "0:3"],
                         capture_output=True, text=True)
    lines = [l.split(None, 2) for l in out.stdout.splitlines()]
    if len(lines) != 2:
        print(f"PSEUDO-01: self-test FAIL expected 2 instructions, got "
              f"{len(lines)}:\n{out.stdout}{out.stderr[:500]}", file=sys.stderr)
        return 1
    fails = 0
    if lines[0][1] != "-" or lines[0][2] != "nop":
        print(f"PSEUDO-01: self-test FAIL nop should not be pseudo: {lines[0]}",
              file=sys.stderr)
        fails += 1
    else:
        print("PSEUDO-01: self-test ok  a translated instruction names no function")
    if not lines[1][1].startswith("__asm_") or lines[1][2] != "cpuid":
        print(f"PSEUDO-01: self-test FAIL cpuid should be pseudo: {lines[1]}",
              file=sys.stderr)
        fails += 1
    else:
        print(f"PSEUDO-01: self-test ok  an untranslated instruction names "
              f"{lines[1][1]}")

    # The exclusion list is read out of the product; an empty one would
    # silently turn the rate into "all pseudo-asm counts", which is a
    # different gate from the one the output claims to be.
    mod = modelled_names()
    if "__asm_hlt" not in mod or "__asm_rep_stosq_memset" not in mod:
        print(f"PSEUDO-01: self-test FAIL the intended-model list did not parse "
              f"out of semantic_recovery_export.cpp: {sorted(mod)}", file=sys.stderr)
        fails += 1
    else:
        print(f"PSEUDO-01: self-test ok  the intended-model list parsed: "
              f"{', '.join(sorted(mod))}")

    if fails:
        return 1
    print("PSEUDO-01: self-test OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--arch", default="")
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--min-rate", type=float, default=None)
    ap.add_argument("--arch-min", default="")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    probe = Path(args.probe)
    if not probe.is_file():
        print(f"PSEUDO-01: probe not found at {probe}", file=sys.stderr)
        return 2

    if args.self_test:
        return self_test(probe)
    if not args.corpus:
        ap.error("--corpus is required unless --self-test is given")

    wanted = [a for a in ARCHES if not args.arch or a in args.arch.split()]
    floors = {}
    for tok in args.arch_min.split():
        k, _, v = tok.partition("=")
        floors[k] = float(v)

    modelled = modelled_names()
    print("PSEUDO-01: intended models, not counted against the rate: "
          + (", ".join(sorted(modelled)) if modelled else "(none found)"))
    print(f"{'arch':9s} {'translated':>11s} {'modelled':>9s} {'unmodelled':>11s} "
          f"{'rate':>8s}  kinds")
    findings = {}
    rates = {}
    for arch in wanted:
        total, kinds = measure(Path(args.corpus), probe, arch)
        nmod = sum(c for (fn, _m), c in kinds.items() if fn in modelled)
        nunmod = sum(c for (fn, _m), c in kinds.items() if fn not in modelled)
        rate = (total - nunmod) / total if total else 0.0
        rates[arch] = rate
        unmod_kinds = {k: c for k, c in kinds.items() if k[0] not in modelled}
        findings[arch] = (unmod_kinds, total)
        print(f"{arch:9s} {total:11d} {nmod:9d} {nunmod:11d} {rate:8.4f}  "
              f"{len(unmod_kinds)}")

    for arch, (kinds, total) in findings.items():
        if not kinds or args.top <= 0:
            continue
        print(f"\n  {arch}: most frequent unmodelled pseudo-asm, of {total} translated")
        for (fn, mnem), c in sorted(kinds.items(), key=lambda kv: -kv[1])[:args.top]:
            pct = 100.0 * c / total if total else 0.0
            print(f"    {mnem:<18s} {fn:<30s} {c:8d}  {pct:5.2f}%")

    if args.min_rate is None and not floors:
        print("\nPSEUDO-01: measurement only -- no --min-rate or --arch-min "
              "given, nothing was gated")
        return 0

    bad = []
    for arch, rate in rates.items():
        floor = floors.get(arch, args.min_rate)
        if floor is None:
            continue
        # Compared at the precision the table prints, so a floor set from a
        # printed number cannot fail on a digit nobody was shown.
        if float(f"{rate:.4f}") < float(f"{floor:.4f}"):
            bad.append(f"{arch} {rate:.4f} < {floor:.4f}")
    if bad:
        print("\nPSEUDO-01: FAIL " + "; ".join(bad), file=sys.stderr)
        return 1
    print("\nPSEUDO-01: OK every architecture met its floor")
    return 0


if __name__ == "__main__":
    sys.exit(main())
