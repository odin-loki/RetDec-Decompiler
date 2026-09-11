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
    "arm":     ("CS_ARCH_ARM",   "CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN"),
    "arm64":   ("CS_ARCH_ARM64", "CS_MODE_LITTLE_ENDIAN"),
    "mips":    ("CS_ARCH_MIPS",  "CS_MODE_MIPS32 | CS_MODE_BIG_ENDIAN"),
    "powerpc": ("CS_ARCH_PPC",   "CS_MODE_32 | CS_MODE_BIG_ENDIAN"),
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
    init = ROOT / f"src/capstone2llvmir/{arch}/{arch}_init.cpp"
    s = init.read_text(encoding="utf-8")
    body = s[s.index("_i2fm"):]
    impl, null = set(), set()
    for m in re.finditer(r'^\s*\{([A-Z0-9_]*INS_[A-Z0-9_]+),\s*(nullptr|&[^}]+)\}', body, re.M):
        (null if m.group(2) == "nullptr" else impl).add(m.group(1))
    return impl, null


def build(src: str, out: Path, inc: Path, lib: Path) -> None:
    p = out.with_suffix(".c")
    p.write_text(src, encoding="utf-8")
    r = subprocess.run(["gcc", str(p), f"-I{inc}", f"-L{lib}", "-lcapstone", "-o", str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"COV-01: FAIL could not build helper:\n{r.stderr[:2000]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--capstone-prefix", required=True)
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--arch", default=",".join(ARCHES))
    args = ap.parse_args()

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

    dispatch = "\n    ".join(
        ('else ' if i else '') +
        f'if (!strcmp(argv[2], "{a}")) {{ arch = {ARCHES[a][0]}; mode = (cs_mode)({ARCHES[a][1]}); }}'
        for i, a in enumerate(wanted))
    dis = work / "dis"
    build(DIS_C % {"dispatch": dispatch}, dis, inc, lib)

    print(f"{'arch':9s} {'decoded':>10s} {'skipped':>9s} {'covered':>10s} {'rate':>8s}  uncovered-kinds")
    findings = {}
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

        counts, skipped, total = Counter(), 0, 0
        for b in sorted(corpus.glob(f"*-{arch}-gcc-*")):
            if not b.is_file():
                continue
            rs = subprocess.run(["readelf", "-S", str(b)], capture_output=True, text=True).stdout
            m = re.search(r'\.text\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s*\n?\s*([0-9a-f]+)', rs)
            if not m:
                continue
            off, size = int(m.group(2), 16), int(m.group(3), 16)
            out = subprocess.run([str(dis), str(b), arch, "0", f"{off}:{size}"],
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
        print(f"{arch:9s} {total:10d} {skipped:9d} {covered:10d} {rate:8.4f}  {len(unc)}")
        findings[arch] = (unc, idmap, total)

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

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
