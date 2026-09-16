#!/usr/bin/env python3
"""COV-01 — what fraction of the instructions in real binaries each translator
actually translates.

Why this exists
---------------
Asked to bring every architecture to parity with x86-64, the obvious number is
the size of each translator's dispatch table. It is the wrong number, and
badly so. Counting entries mapped to a function against entries listed:

    x86 24.6%, arm 40.2%, arm64 41.0%, mips 22.6%, powerpc 23.7%

which says x86-64 -- the one architecture with 216 binaries of end-to-end
evidence behind it -- is the *worst* covered of the five. It is an artefact:
x86 lists every SSE and AVX form it does not translate, so the denominator is
enormous. A ratio over an ISA's own instruction count cannot be compared
across ISAs.

The number that means something is weighted by what real binaries contain.
This disassembles a corpus and asks, of the instructions actually present, how
many the translator has a function for.

x86-64 is measured here too, on the same corpus and the same footing. Leaving
it out was how "is every architecture at x86-64's level" came to be asked
without ever asking it of x86-64: on floating-point programs its own coverage
was the lowest of the five, because ADDSD, SUBSD, MULSD, DIVSD, UCOMISD,
SQRTSD and the SSE form of MOVSD -- the whole double-precision half of SSE2 --
had no translator. A baseline nobody measures is an assumption.

Reading the output
------------------
`skipped` is bytes Capstone could not decode at all. It is the honesty column.
For a fixed-width ISA in a single mode it is near zero and the coverage figure
is trustworthy. For 32-bit ARM it is large, because static glibc interleaves
ARM and Thumb and this disassembles in one mode: the Thumb regions decode as
garbage, much of it as coprocessor instructions that are not in the binary at
all. An ARM coverage figure from this tool is a lower bound with a wide error
bar, and its uncovered list should not be used to decide what to implement.

Usage:
  python3 scripts/ci/check_instruction_coverage.py --corpus DIR
      --capstone-prefix DIR [--top N] [--arch LIST]
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# arch -> (capstone cs_arch, cs_mode expression) for the way the corpus is built
# by scripts/build_multiarch_corpus.sh.
ARCHES = {
    "x86_64":  ("CS_ARCH_X86",   "CS_MODE_64 | CS_MODE_LITTLE_ENDIAN"),
    "arm":     ("CS_ARCH_ARM",   "CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN"),
    "arm64":   ("CS_ARCH_ARM64", "CS_MODE_LITTLE_ENDIAN"),
    "mips":    ("CS_ARCH_MIPS",  "CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN"),
    "powerpc": ("CS_ARCH_PPC",   "CS_MODE_32 | CS_MODE_BIG_ENDIAN"),
}

# The corpus names an architecture the way its cross toolchain is spelled; the
# translator directory is named after the capstone architecture. Only x86
# differs, and it differs because the corpus has to say "x86_64" to pick a
# compiler driver while src/capstone2llvmir/x86 covers 16, 32 and 64 bits.
TRANSLATOR_DIR = {"x86_64": "x86"}

# Not an architecture to report: a second mode the ARM walker below switches
# into. src/capstone2llvmir/arm is one translator serving both, so its numbers
# belong on the `arm` row.
DIS_EXTRA_MODES = {
    "arm_thumb": ("CS_ARCH_ARM", "CS_MODE_THUMB | CS_MODE_LITTLE_ENDIAN"),
}

DIS_C = r'''
#include <capstone/capstone.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    if (argc < 5) return 2;
    cs_arch arch; cs_mode mode;
    %(dispatch)s
    else return 2;
    long off = 0, sz = 0;
    if (sscanf(argv[4], "%%ld:%%ld", &off, &sz) != 2) return 2;
    FILE* f = fopen(argv[1], "rb"); if (!f) return 2;
    unsigned char* buf = malloc(sz);
    fseek(f, off, SEEK_SET);
    sz = fread(buf, 1, sz, f); fclose(f);
    csh h;
    if (cs_open(arch, mode, &h) != CS_ERR_OK) return 2;
    /* Without SKIPDATA the first undecodable word ends the whole run, which
       silently turns a 226 KB .text into five instructions. */
    cs_option(h, CS_OPT_SKIPDATA, CS_OPT_ON);
    cs_insn* insn;
    size_t n = cs_disasm(h, buf, sz, 0x1000, 0, &insn);
    for (size_t i = 0; i < n; i++) printf("%%u %%s\n", insn[i].id, insn[i].mnemonic);
    if (n) cs_free(insn, n);
    cs_close(&h); free(buf);
    return 0;
}
'''


def translator_sets(arch: str) -> tuple[set[str], set[str]]:
    """Instruction enum names the translator implements, and those it lists
    with a null function pointer."""
    d = TRANSLATOR_DIR.get(arch, arch)
    init = ROOT / f"src/capstone2llvmir/{d}/{d}_init.cpp"
    s = init.read_text(encoding="utf-8")
    body = s[s.index("_i2fm"):]
    impl, null = set(), set()
    for m in re.finditer(r'^\s*\{([A-Z0-9_]*INS_[A-Z0-9_]+),\s*(nullptr|&[^}]+)\}', body, re.M):
        (null if m.group(2) == "nullptr" else impl).add(m.group(1))
    return impl, null


def text_section(binary: Path) -> tuple[int, int, int, int] | None:
    """(index, addr, file offset, size) of .text, or None."""
    out = subprocess.run(["readelf", "-SW", str(binary)],
                         capture_output=True, text=True).stdout
    m = re.search(r'\[\s*(\d+)\]\s+\.text\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',
                  out)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)


def arm_regions(binary: Path, ndx: int, addr: int, off: int, size: int):
    """Split .text by the ARM mapping symbols into (file offset, length, mode).

    32-bit ARM ELF carries $a, $t and $d symbols marking runs of ARM code,
    Thumb code and inline data. Without them a single-mode disassembly reads
    the Thumb runs and the literal pools as ARM instructions, which is how
    this tool came to report STC, CDP and LDC -- coprocessor instructions
    that are not in these binaries at all -- as ARM's most frequent
    untranslated kinds. Data regions are dropped rather than counted: they
    are known not to be instructions, which is a different fact from
    capstone failing to decode them.

    Returns (regions, data_bytes). An empty region list means no mapping
    symbols, and the caller falls back to one ARM-mode run.
    """
    out = subprocess.run(["readelf", "-sW", str(binary)],
                         capture_output=True, text=True).stdout
    marks = []
    for line in out.splitlines():
        m = re.match(r'\s*\d+:\s+([0-9a-f]+)\s+\d+\s+NOTYPE\s+LOCAL\s+\S+\s+(\d+)\s+(\$[atd])(?:\.\S*)?\s*$',
                     line)
        if not m:
            continue
        if int(m.group(2)) != ndx:
            continue
        a = int(m.group(1), 16)
        if addr <= a < addr + size:
            marks.append((a, m.group(3)))
    if not marks:
        return [], 0

    marks.sort()
    regions = []
    data = 0
    for i, (a, kind) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else addr + size
        n = end - a
        if n <= 0:
            continue
        if kind == "$d":
            data += n
            continue
        regions.append((off + (a - addr), n, "arm" if kind == "$a" else "arm_thumb"))
    return regions, data


def build(src: str, out: Path, inc: Path, lib: Path) -> None:
    p = out.with_suffix(".c")
    p.write_text(src, encoding="utf-8")
    r = subprocess.run(["gcc", str(p), f"-I{inc}", f"-L{lib}", "-lcapstone", "-o", str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"COV-01: FAIL could not build helper:\n{r.stderr[:2000]}")


def self_test(capstone_prefix: str) -> int:
    """Two binaries, three runs: no floor, an impossible floor, a floor of
    zero. What this proves is that --arch-min is read at all -- the first
    version of this gating returned 0 unconditionally and looked identical
    from the outside on every corpus that happened to be healthy."""
    import shutil

    if not shutil.which("gcc"):
        print("COV-01: FAIL self-test needs gcc", file=sys.stderr)
        return 2
    work = Path(tempfile.mkdtemp(prefix="cov01-self-"))
    corpus = work / "corpus"
    corpus.mkdir()
    src = work / "t.c"
    src.write_text("int main(void){return 0;}\n", encoding="utf-8")
    for n in ("a", "b"):
        r = subprocess.run(["gcc", "-O0", str(src), "-o", str(corpus / f"{n}-x86_64-gcc-O0")],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"COV-01: FAIL self-test could not build a fixture:\n{r.stderr[:500]}",
                  file=sys.stderr)
            return 2

    base = [sys.executable, str(Path(__file__).resolve()),
            "--corpus", str(corpus), "--capstone-prefix", capstone_prefix,
            "--arch", "x86_64"]
    cases = [
        ("no floor is a measurement and exits 0", [], 0),
        ("an unreachable floor fails", ["--arch-min", "x86_64=1.1"], 1),
        ("a floor of zero passes", ["--arch-min", "x86_64=0.0"], 0),
        ("--min-rate alone gates too", ["--min-rate", "1.1"], 1),
    ]
    fails = 0
    for name, extra, want in cases:
        r = subprocess.run(base + extra, capture_output=True, text=True)
        if r.returncode != want:
            print(f"COV-01: self-test FAIL {name}: expected {want}, got {r.returncode}",
                  file=sys.stderr)
            print((r.stdout + r.stderr)[:1000], file=sys.stderr)
            fails += 1
        else:
            print(f"COV-01: self-test ok  {name}")
    if fails:
        return 1
    print("COV-01: self-test OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus")
    ap.add_argument("--capstone-prefix", required=True)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--arch", default=",".join(ARCHES))
    ap.add_argument("--min-rate", type=float, default=None,
                    help="floor applied to every architecture that has no --arch-min entry")
    ap.add_argument("--arch-min", default="",
                    help='per-architecture floors, e.g. "arm64=0.99 mips=1.0"')
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test(args.capstone_prefix)
    if not args.corpus:
        ap.error("--corpus is required unless --self-test is given")

    floors = {}
    for pair in args.arch_min.split():
        if "=" not in pair:
            print(f"COV-01: FAIL --arch-min entry is not arch=rate: {pair}", file=sys.stderr)
            return 2
        k, v = pair.split("=", 1)
        floors[k] = float(v)

    inc = Path(args.capstone_prefix) / "include"
    lib = Path(args.capstone_prefix) / "lib"
    if not (inc / "capstone" / "capstone.h").exists():
        print(f"COV-01: FAIL no capstone headers under {inc}", file=sys.stderr)
        return 2
    corpus = Path(args.corpus)
    if not corpus.is_dir():
        print(f"COV-01: FAIL corpus not found: {corpus}", file=sys.stderr)
        return 2

    work = Path(tempfile.mkdtemp(prefix="cov01-"))
    wanted = [a for a in args.arch.split(",") if a in ARCHES]

    modes = {a: ARCHES[a] for a in wanted}
    if "arm" in wanted:
        modes.update(DIS_EXTRA_MODES)
    dispatch = "\n    ".join(
        ('else ' if i else '') +
        f'if (!strcmp(argv[2], "{a}")) {{ arch = {v[0]}; mode = (cs_mode)({v[1]}); }}'
        for i, (a, v) in enumerate(modes.items()))
    dis = work / "dis"
    build(DIS_C % {"dispatch": dispatch}, dis, inc, lib)

    print(f"{'arch':9s} {'decoded':>10s} {'skipped':>9s} {'covered':>10s} {'rate':>8s}  uncovered-kinds")
    findings = {}
    rates: dict[str, float] = {}
    for arch in wanted:
        impl, _null = translator_sets(arch)
        # enum name -> numeric id, via the same headers the translator compiles
        # against, so the comparison is on ids and not on spelling.
        names = sorted(impl | _null)
        src = ['#include <capstone/capstone.h>', '#include <stdio.h>', 'int main(void){']
        src += [f'  printf("%d {n}\\n", (int){n});' for n in names]
        src += ['  return 0;}']
        idsbin = work / f"ids_{arch}"
        build("\n".join(src), idsbin, inc, lib)
        idmap = {}
        for line in subprocess.run([str(idsbin)], capture_output=True, text=True).stdout.splitlines():
            v, n = line.split(None, 1)
            idmap[int(v)] = n

        counts, skipped, total, datab = Counter(), 0, 0, 0
        for b in sorted(corpus.glob(f"*-{arch}-gcc-*")):
            if not b.is_file():
                continue
            sec = text_section(b)
            if sec is None:
                continue
            ndx, vaddr, off, size = sec
            regions = [(off, size, arch)]
            if arch == "arm":
                r, d = arm_regions(b, ndx, vaddr, off, size)
                if r:
                    regions, datab = r, datab + d
            for roff, rsize, rmode in regions:
                out = subprocess.run([str(dis), str(b), rmode, "0", f"{roff}:{rsize}"],
                                     capture_output=True, text=True).stdout
                for line in out.splitlines():
                    p = line.split(None, 1)
                    if len(p) < 2:
                        continue
                    if p[1].startswith(".byte"):
                        skipped += 1
                        continue
                    total += 1
                    counts[int(p[0])] += 1

        covered = sum(c for i, c in counts.items() if idmap.get(i) in impl)
        unc = sorted(((i, c) for i, c in counts.items() if idmap.get(i) not in impl),
                     key=lambda x: -x[1])
        rate = covered / total if total else 0.0
        note = f"  ({datab} bytes mapped as data)" if datab else ""
        print(f"{arch:9s} {total:10d} {skipped:9d} {covered:10d} {rate:8.4f}  {len(unc)}{note}")
        findings[arch] = (unc, idmap, total)
        rates[arch] = rate

    for arch, (unc, idmap, total) in findings.items():
        if not unc:
            continue
        print(f"\n  {arch}: most frequent untranslated, of {total} decoded")
        for i, c in unc[:args.top]:
            # An id with no entry at all is a different fact from one listed
            # with a null pointer: the first means the table predates the
            # instruction, the second that somebody looked and declined.
            name = idmap.get(i)
            tag = "listed, null" if name else "NO ENTRY"
            print(f"    {(name or f'<id {i}>'):24s} {c:9d}  {100*c/total:5.2f}%  {tag}")

    # Per architecture, because an aggregate cannot see a parity problem: one
    # architecture at 1.00 and another at 0.00 averages to something that
    # reads like "mostly fine" and is not.
    if args.min_rate is None and not floors:
        print("\nCOV-01: measurement only -- no --min-rate or --arch-min given, "
              "nothing was gated")
        return 0
    bad = []
    for arch in wanted:
        if arch not in rates:
            continue
        floor = floors.get(arch, args.min_rate)
        if floor is None:
            continue
        # Compared at the precision it is PRINTED at. The first version of
        # this compared the full double against a floor copied from the
        # table above, and 6003/6045 prints as 0.9931 while being 0.99305,
        # so every architecture failed its own measured floor the moment it
        # was set. A gate whose displayed number and compared number differ
        # is a gate nobody can set a floor for.
        shown = round(rates[arch], 4)
        if shown < floor:
            bad.append(f"{arch}: {shown:.4f} is below the floor {floor:.4f}")
    if bad:
        print("")
        for b in bad:
            print(f"COV-01: FAIL {b}", file=sys.stderr)
        return 1
    print("\nCOV-01: OK every architecture met its floor")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
