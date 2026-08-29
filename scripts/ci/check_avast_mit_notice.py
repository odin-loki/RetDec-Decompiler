#!/usr/bin/env python3
"""Fail if LICENSE-MIT is missing or 2017-2020 Odin Loch rewrite headers remain.

Scans src/, include/, tests/, and docs/doxygen/ (never deps/ or build/).

Usage:
    python3 scripts/ci/check_avast_mit_notice.py
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LICENSE_MIT = REPO_ROOT / "LICENSE-MIT"
SCAN_DIRS = ("src", "include", "tests", "docs/doxygen")
REWRITE_NEEDLES = (
    "copyright (c) 2017 Odin Loch",
    "copyright (c) 2018 Odin Loch",
    "copyright (c) 2019 Odin Loch",
    "copyright (c) 2020 Odin Loch",
)
REQUIRED_MIT_SNIPPETS = (
    'Copyright (c) 2017 Avast Software',
    'Permission is hereby granted, free of charge',
    'THE SOFTWARE IS PROVIDED "AS IS"',
)


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


def main() -> int:
    errors: list[str] = []

    if not LICENSE_MIT.is_file():
        errors.append("LICENSE-MIT is missing from the repository root")
    else:
        try:
            text = LICENSE_MIT.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(f"LICENSE-MIT could not be read: {exc}")
            text = ""
        for snippet in REQUIRED_MIT_SNIPPETS:
            if snippet not in text:
                errors.append(f"LICENSE-MIT is missing required text: {snippet}")

    hits: list[str] = []
    for path in iter_text_files(REPO_ROOT):
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if any(needle in text for needle in REWRITE_NEEDLES):
            hits.append(str(path.relative_to(REPO_ROOT)))

    if hits:
        errors.append(
            f"{len(hits)} file(s) still contain a 2017-2020 Odin Loch rewrite "
            f"header under src/include/tests/docs/doxygen:"
        )
        errors.extend(f"  {p}" for p in hits)

    if errors:
        print("check_avast_mit_notice: FAIL")
        for line in errors:
            print(line)
        return 1

    print("check_avast_mit_notice: OK")
    print("LICENSE-MIT present with Avast MIT permission notice")
    print("no @copyright (c) 2017-2020 Odin Loch under src/include/tests/docs/doxygen")
    return 0


if __name__ == "__main__":
    sys.exit(main())
