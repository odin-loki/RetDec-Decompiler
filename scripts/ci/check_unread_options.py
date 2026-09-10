#!/usr/bin/env python3
"""OPT-01 — no NEW public option field that nothing reads.

Why this exists
---------------
A config field with a default and a doc comment is a promise: set it and
something changes. Several on this branch did not keep it, and each was a real
defect rather than a cosmetic one:

  * StringDetectorConfig::detectWide / detectPascal / detectLenPfx -- three
    documented classification switches that typeString() never saw, so turning
    any of them off changed nothing at all.
  * DVSA::Result::carvedAccesses -- documented as "excluded as ABI-reserved",
    never assigned, so it read 0 on every function including the ones where the
    carving was in fact excluding nothing.
  * AbiArtifactMarker::Config::arm32 / aarch64 -- declared beside win64 and
    sysVAmd64 and never read, so the SysV AMD64 callee-save table was applied
    on every architecture. ARM32's r12 is IP, a caller-saved scratch register,
    and it was being marked a callee-save pair and handed to DCE for removal.

The last one is the argument for this check: the flag that would have
disambiguated r12 already existed, and the bug was that nothing consulted it.

What it does
------------
Finds every field with a default initialiser in a `struct *Config|*Options|
*Params|*Settings` under include/retdec, and asks whether anything under src/
ever reads it. The answer is compared against ACCEPTED, the list of ones known
to be inert today. The check fails when:

  * a field is unread and NOT in ACCEPTED -- a new dead knob, which is what
    this is here to prevent; or
  * a field IS in ACCEPTED but something now reads it -- a stale entry, so the
    list shrinks as knobs get wired up rather than rotting.

ACCEPTED is debt, not absolution. Most entries are switches for features that
were never implemented (there is no text-block emission behind
StmtEmitOptions::emitTextBlocks, and no cast-removal pass behind
CodeGenPass::Stats::castsRemoved). Implementing one, or deleting the field,
both take it off the list; leaving it does not.

Usage: python3 scripts/ci/check_unread_options.py [--check | --write]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ACCEPTED_FILE = ROOT / "scripts/ci/unread_options_accepted.json"

STRUCT_RE = re.compile(
    r'\bstruct\s+(\w*(?:Config|Options|Params|Settings))\s*(?:final\s*)?\{(.*?)\n\s*\};',
    re.S)
# A field with a default. Not `using X = Y;` and not `static constexpr ...`.
FIELD_RE = re.compile(r'^\s*(?:mutable\s+)?(?:const\s+)?([\w:<>,\s\*&]+?)\s+(\w+)\s*=\s*([^;]+);')


def fields() -> list[tuple[str, str, str]]:
    """(header, struct, field) for every option field with a default."""
    out = []
    for h in sorted((ROOT / "include/retdec").rglob("*.h")):
        text = h.read_text(errors="ignore")
        for m in STRUCT_RE.finditer(text):
            struct, body = m.group(1), m.group(2)
            for line in body.split("\n"):
                code = line.split("///")[0].split("//")[0]
                if re.match(r"\s*(using|typedef|static|friend)\b", code):
                    continue
                fm = FIELD_RE.match(code)
                if not fm:
                    continue
                ty, name = fm.group(1).strip(), fm.group(2)
                # Type aliases and nested types are CapitalCase; fields are not.
                if not name[:1].islower() or name.endswith("_"):
                    continue
                if "=" in ty or ty.startswith("using"):
                    continue
                rel = str(h.relative_to(ROOT))
                out.append((rel, struct, name))
    return out


def sources() -> str:
    listed = subprocess.run(
        ["git", "ls-files", "src/*.cpp", "src/*.h", "src/*.cu"],
        cwd=ROOT, capture_output=True, text=True).stdout.split()
    return "\n".join((ROOT / f).read_text(errors="ignore") for f in listed)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    src = sources()
    unread = []
    for rel, struct, name in fields():
        if not re.search(r"[.>]\s*" + re.escape(name) + r"\b", src):
            unread.append(f"{rel}::{struct}::{name}")
    unread_set = set(unread)

    if args.write:
        ACCEPTED_FILE.write_text(
            json.dumps({"accepted": sorted(unread_set)}, indent=1) + "\n",
            encoding="utf-8")
        print(f"OPT-01: wrote {len(unread_set)} accepted entries to "
              f"{ACCEPTED_FILE.relative_to(ROOT)}")
        return 0

    if not ACCEPTED_FILE.exists():
        print(f"OPT-01: FAIL {ACCEPTED_FILE.relative_to(ROOT)} does not exist; "
              f"seed it with --write", file=sys.stderr)
        return 1
    accepted = set(json.loads(ACCEPTED_FILE.read_text(encoding="utf-8"))["accepted"])

    new = sorted(unread_set - accepted)
    stale = sorted(accepted - unread_set)

    if new or stale:
        print("OPT-01: FAIL", file=sys.stderr)
        for f in new:
            print(f"  NEW dead knob: {f}", file=sys.stderr)
            print("    A field with a default that nothing under src/ reads. Wire it "
                  "up, or delete it -- a knob that does nothing is worse than no knob, "
                  "because a caller may set it and believe something happened.",
                  file=sys.stderr)
        for f in stale:
            print(f"  now read, remove from the accepted list: {f}", file=sys.stderr)
        print("  Re-seed with: python3 scripts/ci/check_unread_options.py --write",
              file=sys.stderr)
        return 1

    print(f"OPT-01: OK no new unread option fields "
          f"({len(accepted)} known-inert, unchanged)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
