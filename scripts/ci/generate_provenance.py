#!/usr/bin/env python3
"""LEG-03 — classify src/include/tests headers and write a file-level summary.

Never scans deps/ or build/. Writes docs/PROVENANCE-files.md.

Usage:
    python3 scripts/ci/generate_provenance.py
"""

from __future__ import annotations

from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ("src", "include", "tests")
OUT = REPO_ROOT / "docs" / "PROVENANCE-files.md"
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}

UPSTREAM_MODULES = {
    "llvmir2hll", "bin2llvmir", "fileformat", "fileinfo", "capstone2llvmir",
    "capstone2llvmirtool", "loader", "ctypes", "ctypesparser", "cpdetect",
    "unpacker", "unpackertool", "utils", "common", "config", "debugformat",
    "yaracpp", "pdbparser", "patterngen", "pat2yara", "rtti-finder",
    "stacofin", "stacofintool", "llvmir-emul", "ar-extractor",
    "ar-extractortool", "macho-extractor", "macho-extractortool", "bin2pat",
    "getsig", "idr2pat", "pelib", "demangler", "demanglertool", "retdec",
    "retdectool", "retdec-decompiler", "serdes",
}

AVAST = "2017 Avast Software"
PORST = "Sebastian Porst"
ODIN_REWRITE = tuple(
    f"@copyright (c) {y} Odin Loch" for y in ("2017", "2018", "2019", "2020")
)


def module_of(rel: Path) -> str:
    parts = rel.parts
    if parts[0] == "src" and len(parts) >= 2:
        return parts[1]
    if parts[0] == "include" and len(parts) >= 3 and parts[1] == "retdec":
        return parts[2]
    if parts[0] == "tests" and len(parts) >= 2:
        return parts[1]
    return parts[0]


def classify(text: str) -> str:
    if PORST in text:
        return "pelib-porst"
    if AVAST in text:
        return "avast-mit"
    if any(n in text for n in ODIN_REWRITE):
        return "rewrite-tell-leftover"
    return "imortek-or-undated"


def main() -> int:
    counts: Counter[str] = Counter()
    leftover: list[str] = []
    odin_in_upstream: list[str] = []
    n = 0
    for scan in SCAN_DIRS:
        base = REPO_ROOT / scan
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SUFFIXES:
                continue
            if any(p in {"deps", "build"} for p in path.parts):
                continue
            try:
                text = path.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            n += 1
            kind = classify(text)
            counts[kind] += 1
            rel = path.relative_to(REPO_ROOT).as_posix()
            if kind == "rewrite-tell-leftover":
                leftover.append(rel)
            if kind == "imortek-or-undated" and module_of(path.relative_to(REPO_ROOT)) in UPSTREAM_MODULES:
                odin_in_upstream.append(rel)

    lines = [
        "# Generated provenance summary",
        "",
        "Produced by `scripts/ci/generate_provenance.py`. Do not hand-edit.",
        f"Scanned `{n}` C/C++ files under `src/`, `include/`, `tests/`.",
        "",
        "| Class | Count |",
        "|-------|------:|",
    ]
    for k in ("avast-mit", "pelib-porst", "imortek-or-undated", "rewrite-tell-leftover"):
        lines.append(f"| `{k}` | {counts[k]} |")
    lines += [
        "",
        f"Odin-only files in known-upstream modules: **{len(odin_in_upstream)}** "
        "(Imortek additions inside Avast directories, or undated headers).",
        "",
        "## Rewrite-tell leftovers (must be zero)",
        "",
    ]
    if leftover:
        lines.extend(f"- `{p}`" for p in leftover)
    else:
        lines.append("None.")
    lines += ["", "## Odin-only in upstream modules (first 80)", ""]
    for p in odin_in_upstream[:80]:
        lines.append(f"- `{p}`")
    if len(odin_in_upstream) > 80:
        lines.append(f"- … {len(odin_in_upstream) - 80} more")
    lines.append("")
    OUT.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(REPO_ROOT)} files={n} leftover={len(leftover)}")
    return 1 if leftover else 0


if __name__ == "__main__":
    raise SystemExit(main())
