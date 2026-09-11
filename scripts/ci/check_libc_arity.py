#!/usr/bin/env python3
"""ARITY-01 — check the arity tables against the real C headers.

Why this exists
---------------
`.../libc_semantics/get_arity_of_func.cpp` and
`.../gcc_general_semantics/get_arity_of_func.cpp` tell the C writer how many
parameters a library function takes, and the writer uses it both to drop
arguments the front end's parameter recovery invented and to cast a callee
whose declaration the recovered call is too short for -- so a wrong entry
silently deletes a real argument from the emitted C, or casts a call that did
not need it. The note that recorded this defect said a table "is only worth
adding if it is right", and a hand-written one cannot be shown to be.

There is one table per semantics that assigns C headers to function names,
because assigning a header is what commits the emitted file to that header's
signatures. gcc_general is the one that knows <pthread.h>; until its table
existed, nothing could tell the writer that `pthread_create(thread)` was three
arguments short of the declaration the same file asks for.

So the table is measured, not recalled. For every name in it this script
compiles a call with 0..8 arguments against the header the semantics assigns
that function, and records which counts the real declaration accepts:

  * exactly one accepted count      -> fixed arity, that count
  * everything from N up to the cap -> variadic with N named parameters
  * anything else                   -> not a plain function here (a
                                       function-like macro such as isnan, or
                                       not declared on this platform), and it
                                       must NOT be in the table

Every header the semantics assigns is probed for availability first, because
an absent header and a function-like macro are otherwise the same observation
-- nothing compiles -- and only one of them is a defect in the table. Headers
that are not part of a base toolchain are named in HEADERS_NOT_ASSUMED instead,
so the table cannot come to depend on what happens to be installed.

`--check` (the default) compares the table against that measurement and fails
on any disagreement. `--write` regenerates the file from it.

One flag matters and is easy to get wrong: `-w` inhibits all warnings AND
defeats `-Werror=implicit-function-declaration`, so an undeclared function
accepts every argument count and measures as variadic. `gets`, removed from
C11 glibc, did exactly that on the first run of this script. There is no `-w`
here on purpose.

Usage: python3 scripts/ci/check_libc_arity.py [--check | --write] [--jobs N]
                                             [--module libc|gcc_general]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SEMANTICS = ROOT / "src/llvmir2hll/semantics/semantics"

# Every semantics that assigns C headers to function names, and the arity table
# derived from it. Both are measured the same way and checked the same way.
#
# gcc_general is here because it is the one that knows about <pthread.h>, and
# `pthread_create` is the reason: the corpus emits `pthread_create(thread)`
# under a header that declares four parameters, and nothing could tell the
# writer that until this table existed.
MODULES = [
    (
        "libc",
        SEMANTICS / "libc_semantics/get_c_header_file_for_func.cpp",
        SEMANTICS / "libc_semantics/get_arity_of_func.cpp",
    ),
    (
        "gcc_general",
        SEMANTICS / "gcc_general_semantics/get_c_header_file_for_func.cpp",
        SEMANTICS / "gcc_general_semantics/get_arity_of_func.cpp",
    ),
]
MAX_ARGS = 8

# Headers this check does not assume, and why.
#
# The table has to mean the same thing everywhere it is checked. These are not
# part of the C standard or of glibc's base set -- they arrive with a -dev
# package -- so a machine that has them measures entries a machine without them
# cannot re-derive, and the check fails on the second machine for a table that
# was right on the first. That is exactly what happened: 19 dbm_/gdbm_ entries
# were written here, where libgdbm-dev is installed, and standalone-check run
# 161 rejected every one of them.
#
# The rule is the one standalone_check.sh's EXCLUDED_REASONS already states for
# modules: an entry whose verdict depends on what is installed is an entry this
# check cannot make, so it is named here with the reason rather than left to
# fail somewhere else.
HEADERS_NOT_ASSUMED = {
    "gdbm.h": "provided by libgdbm-dev, which is not part of a base toolchain",
    "ndbm.h": "provided by libgdbm-dev, which is not part of a base toolchain",
    # The other direction, and the header probe is what found these: absent
    # here and on the runner, so the table is right today by accident. An older
    # glibc has them, would measure 11 more entries into the table, and every
    # machine on a current glibc would then reject all 11.
    "libio.h": "glibc made it internal in 2.28; not present on a current glibc",
    "stropts.h": "STREAMS, removed from glibc in 2.30; not present on a current glibc",
}


def func_headers(header_table: Path) -> dict[str, str]:
    """Every function the semantics assigns a C header, and which header.

    Functions whose header is in HEADERS_NOT_ASSUMED are left out entirely, so
    they are neither measured nor expected in the table.
    """
    s = header_table.read_text(encoding="utf-8")
    out: dict[str, str] = {}
    pat = r'static const char \*(\w+)\[\] = \{(.*?)\};\s*ADD_FUNCS_TO_C_HEADER_MAP\(\s*\1\s*,\s*"([^"]+)"'
    for m in re.finditer(pat, s, re.S):
        hdr = m.group(3)
        if hdr in HEADERS_NOT_ASSUMED:
            continue
        for name in re.findall(r'"([^"]+)"', m.group(2)):
            out[name] = hdr
    return out


def parse_arity_table(arity_table: Path) -> dict[str, tuple[int, bool]]:
    s = arity_table.read_text(encoding="utf-8") if arity_table.exists() else ""
    out: dict[str, tuple[int, bool]] = {}
    for m in re.finditer(r'ADD_FUNC_ARITY\("([^"]+)",\s*(\d+),\s*(true|false)\)', s):
        out[m.group(1)] = (int(m.group(2)), m.group(3) == "true")
    return out


def probe_headers(hdrs: list[str], jobs: int) -> set[str]:
    """Which of these headers this toolchain actually has.

    Without this, an absent header is indistinguishable from a function-like
    macro: both make every arg count fail to compile, so both classify as "not
    measurable" and every name under the header is reported as a bad table
    entry. That is what standalone-check runs 152-161 printed -- 19 lines
    blaming dbm_/gdbm_ entries, when the one fact worth knowing was that the
    runner has no <gdbm.h>. Ten runs, one cause, and the message never said it.
    """
    work = tempfile.mkdtemp(prefix="libc-arity-hdr-")
    cc = os.environ.get("CC", "gcc")

    def has(job) -> tuple[str, bool]:
        hdr, idx = job
        src = os.path.join(work, f"h{idx}.c")
        with open(src, "w", encoding="utf-8") as f:
            f.write(f"#include <{hdr}>\n")
        r = subprocess.run([cc, "-std=c11", "-fsyntax-only", src],
                           capture_output=True)
        return hdr, r.returncode == 0

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        return {h for h, ok in ex.map(has, [(h, i) for i, h in enumerate(hdrs)]) if ok}


def measure(items: list[tuple[str, str]], jobs: int) -> dict[str, tuple[int, bool] | None]:
    work = tempfile.mkdtemp(prefix="libc-arity-")
    cc = os.environ.get("CC", "gcc")

    def accepts(hdr: str, fn: str, n: int, idx: int) -> bool:
        src = os.path.join(work, f"t{idx}.c")
        args = ",".join("0" for _ in range(n))
        with open(src, "w", encoding="utf-8") as f:
            f.write(f"#include <{hdr}>\nvoid t(void){{ {fn}({args}); }}\n")
        # No -w. See the module docstring.
        r = subprocess.run(
            [cc, "-std=c11", "-fsyntax-only",
             "-Werror=implicit-function-declaration", "-Werror=implicit-int", src],
            capture_output=True)
        return r.returncode == 0

    def classify(job):
        (fn, hdr), idx = job
        acc = [n for n in range(MAX_ARGS + 1) if accepts(hdr, fn, n, idx)]
        if not acc:
            return fn, None
        lo, hi = acc[0], acc[-1]
        if acc != list(range(lo, hi + 1)):
            return fn, None
        if hi == MAX_ARGS and lo != MAX_ARGS:
            return fn, (lo, True)
        if lo == hi:
            return fn, (lo, False)
        return fn, None

    res: dict[str, tuple[int, bool] | None] = {}
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for fn, r in ex.map(classify, [(kv, i) for i, kv in enumerate(items)]):
            res[fn] = r
    return res


def render(measured: dict[str, tuple[int, bool] | None], headers: dict[str, str],
           arity_table: Path) -> str:
    by_hdr: dict[str, list] = {}
    for fn, r in sorted(measured.items()):
        if r is None:
            continue
        by_hdr.setdefault(headers[fn], []).append((fn, r[0], r[1]))
    body_lines = []
    for h in sorted(by_hdr):
        body_lines.append(f"\t// {h}")
        for fn, n, v in sorted(by_hdr[h]):
            body_lines.append(f'\tADD_FUNC_ARITY("{fn}", {n}, {"true" if v else "false"});')
        body_lines.append("")
    body = "\n".join(body_lines).rstrip()
    template = arity_table.read_text(encoding="utf-8")
    start = template.index("\tstatic FuncArityMap m;\n") + len("\tstatic FuncArityMap m;\n")
    end = template.index("\n\treturn m;\n}", start)
    return template[:start] + "\n" + body + template[end:]


def run_module(label: str, header_table: Path, arity_table: Path,
               write: bool, jobs: int) -> int:
    headers = func_headers(header_table)
    if not headers:
        print(f"ARITY-01: FAIL could not read any function from "
              f"{header_table.relative_to(ROOT)}", file=sys.stderr)
        return 1

    # Before measuring anything: a header the semantics assigns but this
    # toolchain does not have makes every name under it unmeasurable, and the
    # per-function message for that is wrong and misleading. Say it once, here,
    # in the only terms that identify the cause. This gates --write too, so a
    # machine missing a base header cannot quietly write a smaller table.
    assigned = sorted(set(headers.values()))
    absent = [h for h in assigned if h not in probe_headers(assigned, jobs)]
    if absent:
        print(f"ARITY-01: FAIL {label}: {len(absent)} header(s) the semantics "
              f"assigns are not available to this toolchain", file=sys.stderr)
        for h in absent:
            n = sum(1 for v in headers.values() if v == h)
            print(f"  <{h}>: assigned to {n} function(s), cannot be measured here",
                  file=sys.stderr)
        print("  The table must mean the same thing on every machine that checks "
              "it, so either install the header or, if it is not part of a base "
              "toolchain, name it in HEADERS_NOT_ASSUMED with the reason.",
              file=sys.stderr)
        return 1

    measured = measure(sorted(headers.items()), jobs)

    if write:
        arity_table.write_text(render(measured, headers, arity_table), encoding="utf-8")
        kept = sum(1 for v in measured.values() if v is not None)
        print(f"ARITY-01: wrote {kept} entries to {arity_table.relative_to(ROOT)}")
        return 0

    table = parse_arity_table(arity_table)
    want = {fn: v for fn, v in measured.items() if v is not None}

    wrong = [(fn, table[fn], want[fn]) for fn in sorted(want) if fn in table and table[fn] != want[fn]]
    missing = [fn for fn in sorted(want) if fn not in table]
    extra = [fn for fn in sorted(table) if fn not in want]

    if wrong or missing or extra:
        print(f"ARITY-01: FAIL the {label} arity table disagrees with the C headers",
              file=sys.stderr)
        for fn, got, exp in wrong[:20]:
            print(f"  {fn}: table says {got[0]} params variadic={got[1]}, "
                  f"the header says {exp[0]} params variadic={exp[1]}", file=sys.stderr)
        for fn in missing[:20]:
            print(f"  {fn}: measurable from <{headers[fn]}> but absent from the table", file=sys.stderr)
        for fn in extra[:20]:
            print(f"  {fn}: in the table but NOT measurable here -- a macro, or not "
                  f"declared on this platform; an entry that cannot be checked "
                  f"must not be in the table", file=sys.stderr)
        print("  Re-derive with: python3 scripts/ci/check_libc_arity.py --write", file=sys.stderr)
        return 1

    print(f"ARITY-01: OK {label}: {len(table)} entries match the system C headers "
          f"({len(headers) - len(want)} names left out as macros or undeclared)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="regenerate the tables")
    ap.add_argument("--check", action="store_true", help="compare (the default)")
    ap.add_argument("--jobs", type=int, default=min(16, (os.cpu_count() or 4)))
    ap.add_argument("--module", help="only this semantics (libc, gcc_general)")
    args = ap.parse_args()

    status = 0
    ran = 0
    for label, header_table, arity_table in MODULES:
        if args.module and args.module != label:
            continue
        ran += 1
        status |= run_module(label, header_table, arity_table, args.write, args.jobs)

    if not ran:
        print(f"ARITY-01: FAIL no such module: {args.module}", file=sys.stderr)
        return 1

    return status


if __name__ == "__main__":
    raise SystemExit(main())
