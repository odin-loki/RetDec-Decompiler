#!/usr/bin/env python3
"""CI-10 — fail if high-confidence secret material is committed.

Scans tracked-looking text under the repo (not deps/, build/, .git/).
This is a local pattern gate, not GitHub's native secret-scanning product.

Usage:
    python3 scripts/ci/check_secrets.py
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SKIP_DIR_NAMES = frozenset(
    {
        ".git",
        "deps",
        "build",
        "node_modules",
        "__pycache__",
        ".venv",
    }
)

TEXT_SUFFIXES = frozenset(
    {
        ".c",
        ".cc",
        ".cpp",
        ".cxx",
        ".h",
        ".hh",
        ".hpp",
        ".hxx",
        ".py",
        ".sh",
        ".ps1",
        ".cmake",
        ".yml",
        ".yaml",
        ".md",
        ".txt",
        ".in",
        ".json",
        ".xml",
        ".toml",
        ".ini",
        ".cfg",
        ".env",
        ".pem",
        ".key",
    }
)

PATTERNS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("aws-access-key", re.compile(r"AKIA[0-9A-Z]{16}")),
    ("github-token", re.compile(r"ghp_[A-Za-z0-9]{36}")),
    ("github-fine-grained", re.compile(r"github_pat_[A-Za-z0-9_]{22,}")),
    ("private-key", re.compile(r"-----BEGIN (?:RSA |OPENSSH |EC |DSA )?PRIVATE KEY-----")),
    ("slack-token", re.compile(r"xox[baprs]-[A-Za-z0-9-]{10,}")),
)

MAX_BYTES = 1_000_000


def iter_scan_files() -> list[Path]:
    out: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(REPO_ROOT):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_NAMES]
        for name in filenames:
            path = Path(dirpath) / name
            if path.suffix.lower() not in TEXT_SUFFIXES and path.name not in {
                "CMakeLists.txt",
                "Dockerfile",
            }:
                continue
            out.append(path)
    return out


def main() -> int:
    hits: list[str] = []
    for path in iter_scan_files():
        try:
            size = path.stat().st_size
        except OSError:
            continue
        if size > MAX_BYTES:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        for kind, pat in PATTERNS:
            if pat.search(text):
                hits.append(f"{rel}: {kind}")
    if hits:
        print("check_secrets: FAIL")
        for h in hits:
            print(f"  {h}")
        return 1
    print("check_secrets: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
