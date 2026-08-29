#!/usr/bin/env python3
"""DOC-05 — public docs must not re-assert withdrawn CLAIMS.md IDs as current.

Scans README.md, docs/*.md (not docs/internal/), WHITEPAPER.
CLAIMS.md itself is the register and is skipped.

A withdrawn ID may appear only when the same line also contains a
withdrawal marker (withdrawn, historical, unintegrated, not wired, …).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CLAIMS = REPO_ROOT / "docs" / "CLAIMS.md"
ID_RE = re.compile(r"\b(C-[A-Z0-9-]+)\b")
ROW_RE = re.compile(
    r"^\|\s*(C-[A-Z0-9-]+)\s*\|.*\|\s*(withdrawn|demonstrated|unpublished|opt-in|asserted)\s*\|",
    re.IGNORECASE,
)
MARKERS = (
    "withdrawn",
    "historical",
    "unintegrated",
    "not wired",
    "do not republish",
    "do not advertise",
    "not a product",
    "asserts",
)


def public_doc_files() -> list[Path]:
    files: list[Path] = []
    seen: set[Path] = set()

    def add(path: Path) -> None:
        if not path.is_file():
            return
        key = path.resolve()
        if key in seen:
            return
        seen.add(key)
        files.append(path)

    add(REPO_ROOT / "README.md")
    docs = REPO_ROOT / "docs"
    if docs.is_dir():
        for path in sorted(docs.iterdir()):
            if path.is_file() and path.suffix.lower() == ".md":
                add(path)
    for name in ("WHITEPAPER.md", "WHITEPAPER"):
        add(REPO_ROOT / name)
    for rel in (
        "QUICKSTART.md",
        "SECURITY.md",
        "CLA.md",
        "CONTRIBUTING.md",
        "LICENSING_FAQ.md",
        "CODE_OF_CONDUCT.md",
        "releases/README.md",
        "scripts/README.md",
        "ROADMAP.md",
    ):
        add(REPO_ROOT / rel)
    return files


def withdrawn_ids() -> set[str]:
    text = CLAIMS.read_text(encoding="utf-8", errors="replace")
    found: set[str] = set()
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if m and m.group(2).lower() == "withdrawn":
            found.add(m.group(1))
    return found


def line_ok(line: str) -> bool:
    lower = line.lower()
    return any(m in lower for m in MARKERS)


def main() -> int:
    withdrawn = withdrawn_ids()
    if not withdrawn:
        print("check_withdrawn_claims: FAIL (no withdrawn IDs parsed from CLAIMS.md)")
        return 1
    print("check_withdrawn_claims: withdrawn IDs:", ", ".join(sorted(withdrawn)))
    errors: list[str] = []
    for path in public_doc_files():
        if path.resolve() == CLAIMS.resolve():
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        for i, line in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            for cid in ID_RE.findall(line):
                if cid not in withdrawn:
                    continue
                if line_ok(line):
                    continue
                errors.append(f"{rel}:{i}: {cid} without a withdrawal marker")
    if errors:
        print("check_withdrawn_claims: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    print("check_withdrawn_claims: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
