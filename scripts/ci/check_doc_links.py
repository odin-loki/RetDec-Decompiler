#!/usr/bin/env python3
"""DOC-06 — every relative link in a Markdown file resolves to a real path.

The doc gate checks a lot about the prose and nothing about whether its
cross-references point anywhere. A sweep at the time this was written found 44
broken relative targets across the tree, almost all of them wrong-depth `../`
prefixes between docs/ and docs/internal/ -- and one, docs/pipeline_stage_map.md's
DECOMPILATION_IMPROVEMENT_FRAMEWORK.md, that never existed. That mattered more
than a dead link: every one of the 29 stages in that table is marked
"Implemented" and the header defers the caveats to the two documents it links,
so the missing target removed the only stated qualification on the table.

External links (http, https, mailto) and pure fragments are not checked -- that
needs the network, and a link check that needs the network is a link check that
gets turned off.

Usage:
    python3 scripts/ci/check_doc_links.py
    python3 scripts/ci/check_doc_links.py --self-test
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SKIP_DIRS = frozenset({".git", "build", "node_modules", "__pycache__", "deps"})

# [text](target) and [text](target "title"). Angle-bracket targets and
# reference-style links are not used in this tree.
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")

EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "ftp://", "#")


def markdown_files(root: Path):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for name in sorted(filenames):
            if name.endswith(".md"):
                yield Path(dirpath) / name


def broken_links(root: Path):
    """Yield (path, line, target) for every relative link that resolves nowhere."""
    for path in markdown_files(root):
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in LINK_RE.finditer(text):
            target = match.group(1)
            if target.startswith(EXTERNAL_PREFIXES):
                continue
            # Strip a fragment: the file has to exist, the anchor is not checked.
            base = target.split("#", 1)[0]
            if not base:
                continue
            resolved = (path.parent / base).resolve()
            if not resolved.exists():
                line = text.count("\n", 0, match.start()) + 1
                yield path, line, target


def self_test() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "docs").mkdir()
        (root / "docs" / "real.md").write_text("real\n", encoding="utf-8")
        (root / "index.md").write_text(
            "[ok](docs/real.md)\n"
            "[ok anchor](docs/real.md#section)\n"
            "[external](https://example.invalid/x.md)\n"
            "[fragment](#here)\n"
            "[dead](docs/missing.md)\n"
            "[wrong depth](../real.md)\n",
            encoding="utf-8",
        )
        found = sorted(t for _, _, t in broken_links(root))
        expected = ["../real.md", "docs/missing.md"]
        if found != expected:
            print(f"check_doc_links --self-test: FAIL: {found} != {expected}")
            return 1
    print("check_doc_links --self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    found = list(broken_links(REPO_ROOT))
    checked = sum(
        1
        for path in markdown_files(REPO_ROOT)
        for m in LINK_RE.finditer(path.read_text(encoding="utf-8", errors="replace"))
        if not m.group(1).startswith(EXTERNAL_PREFIXES) and m.group(1).split("#", 1)[0]
    )

    for path, line, target in found:
        rel = path.relative_to(REPO_ROOT)
        print(f"{rel}:{line}: link target does not exist: {target}")

    print(f"check_doc_links: checked {checked} relative link(s)")
    if found:
        print(f"check_doc_links: FAIL: {len(found)} broken link(s)")
        return 1
    print("check_doc_links: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
