#!/usr/bin/env python3
"""Restore Avast MIT copyright on mechanically rewritten Imortek headers.

Walks src/, include/, and tests/ (never deps/ or build/). Replaces
@copyright (c) YEAR Odin Loch rewrite tells (Avast-era years 2017–2020)
with the upstream Avast line plus a 2025-2026 Imortek modifications line.

Skips files that retain Sebastian Porst copyright (pelib originals) and
does not touch files that already correctly attribute Avast unless they
still contain a leftover rewrite line for the selected years.

Usage:
    python3 scripts/ci/restore_avast_headers.py [--dry-run] [--year 2018]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ("src", "include", "tests")

ALLOWED_YEARS = ("2017", "2018", "2019", "2020")

AVAST_LINE = "@copyright (c) 2017 Avast Software, licensed under the MIT license"
IMORTEK_LINE = "@copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)"

PORST_MARKER = "Sebastian Porst"
AVAST_OK_MARKER = "2017 Avast Software"


def rewrite_pattern(years: tuple[str, ...]) -> re.Pattern[str]:
    year_alt = "|".join(years)
    return re.compile(
        r"^((?:[ \t]*//|[ \t]*\*)?[ \t]*)"
        rf"@copyright \(c\) ({year_alt}) Odin Loch\b.*$"
    )


def rewrite_marker(year: str) -> str:
    return f"@copyright (c) {year} Odin Loch"


def iter_text_files(root: Path):
    for rel in SCAN_DIRS:
        base = root / rel
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file():
                continue
            if any(part in {"deps", "build"} for part in path.parts):
                continue
            yield path


def read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        return None


def restore_text(text: str, pattern: re.Pattern[str]) -> tuple[str, int]:
    rewritten = 0
    out_lines: list[str] = []
    for line in text.splitlines(keepends=True):
        ending = ""
        body = line
        if body.endswith("\r\n"):
            ending = "\r\n"
            body = body[:-2]
        elif body.endswith("\n"):
            ending = "\n"
            body = body[:-1]
        match = pattern.match(body)
        if match:
            prefix = match.group(1)
            out_lines.append(f"{prefix}{AVAST_LINE}{ending}")
            out_lines.append(f"{prefix}{IMORTEK_LINE}{ending}")
            rewritten += 1
        else:
            out_lines.append(line)
    return "".join(out_lines), rewritten


def top_src_module(rel: Path) -> str | None:
    parts = rel.parts
    if len(parts) >= 2 and parts[0] == "src":
        return parts[1]
    if len(parts) >= 3 and parts[0] == "include" and parts[1] == "retdec":
        return parts[2]
    if len(parts) >= 2 and parts[0] == "tests":
        return parts[1]
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="report files that would change without writing",
    )
    parser.add_argument(
        "--year",
        action="append",
        choices=ALLOWED_YEARS,
        help="rewrite tell year to restore (repeatable; default: all 2017-2020)",
    )
    args = parser.parse_args()
    years = tuple(args.year) if args.year else ALLOWED_YEARS
    pattern = rewrite_pattern(years)
    markers = [rewrite_marker(y) for y in years]

    rewritten_files = 0
    rewritten_lines = 0
    skipped_porst = 0
    skipped_already_avast = 0
    leftover = []
    module_counts: Counter[str] = Counter()

    for path in iter_text_files(REPO_ROOT):
        text = read_text(path)
        if text is None:
            continue
        if PORST_MARKER in text:
            skipped_porst += 1
            continue
        if not any(m in text for m in markers):
            if AVAST_OK_MARKER in text:
                skipped_already_avast += 1
            continue
        new_text, n = restore_text(text, pattern)
        if n == 0:
            leftover.append(path)
            continue
        rewritten_files += 1
        rewritten_lines += n
        rel = path.relative_to(REPO_ROOT)
        module = top_src_module(rel)
        if module:
            module_counts[module] += 1
        if not args.dry_run:
            path.write_text(new_text, encoding="utf-8", newline="")

    leftover_after = []
    for path in iter_text_files(REPO_ROOT):
        text = read_text(path)
        if text is None:
            continue
        if any(m in text for m in markers):
            leftover_after.append(str(path.relative_to(REPO_ROOT)))

    mode = "dry-run" if args.dry_run else "applied"
    print(f"mode: {mode}")
    print(f"years: {','.join(years)}")
    print(f"files_rewritten: {rewritten_files}")
    print(f"copyright_lines_replaced: {rewritten_lines}")
    print(f"skipped_sebastian_porst: {skipped_porst}")
    print(f"skipped_already_avast_no_rewrite: {skipped_already_avast}")
    print(f"leftover_odin_loch_for_selected_years: {len(leftover_after)}")
    if leftover_after:
        print("leftover_paths:")
        for p in leftover_after:
            print(f"  {p}")
    print("modules_rewritten:")
    for name, count in sorted(module_counts.items()):
        print(f"  {name}: {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
