#!/usr/bin/env python3
"""REL-04 — CMakeLists.txt, releases/VERSION, and CHANGELOG heading must match.

Usage:
    python3 scripts/ci/check_version_drift.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CMAKE = REPO_ROOT / "CMakeLists.txt"
RELEASES = REPO_ROOT / "releases" / "VERSION"
CHANGELOG = REPO_ROOT / "CHANGELOG.md"

CMAKE_RE = re.compile(r"^\s*VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)
CHANGELOG_RE = re.compile(r"^## \[([0-9]+\.[0-9]+\.[0-9]+)\]", re.MULTILINE)
RELEASES_RE = re.compile(r"^version=([0-9]+\.[0-9]+\.[0-9]+)\s*$", re.MULTILINE)


def main() -> int:
    cmake_text = CMAKE.read_text(encoding="utf-8", errors="replace")
    rel_text = RELEASES.read_text(encoding="utf-8-sig", errors="replace")
    log_text = CHANGELOG.read_text(encoding="utf-8", errors="replace")

    cmake_m = CMAKE_RE.search(cmake_text)
    rel_m = RELEASES_RE.search(rel_text)
    log_m = CHANGELOG_RE.search(log_text)

    errors: list[str] = []
    if not cmake_m:
        errors.append("CMakeLists.txt: no project VERSION x.y.z")
    if not rel_m:
        errors.append("releases/VERSION: no version=x.y.z")
    if not log_m:
        errors.append("CHANGELOG.md: no ## [x.y.z] heading")

    cmake_v = cmake_m.group(1) if cmake_m else None
    rel_v = rel_m.group(1) if rel_m else None
    log_v = log_m.group(1) if log_m else None
    print(f"check_version_drift: cmake={cmake_v} releases={rel_v} changelog={log_v}")

    if cmake_v and rel_v and cmake_v != rel_v:
        errors.append(f"CMakeLists.txt VERSION {cmake_v} != releases/VERSION {rel_v}")
    if cmake_v and log_v and cmake_v != log_v:
        errors.append(f"CMakeLists.txt VERSION {cmake_v} != CHANGELOG {log_v}")
    if rel_v and log_v and rel_v != log_v:
        errors.append(f"releases/VERSION {rel_v} != CHANGELOG {log_v}")

    if errors:
        print("check_version_drift: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    print("check_version_drift: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
