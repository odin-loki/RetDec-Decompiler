#!/usr/bin/env python3
"""E9 — advertised feature tokens must exist as symbols in the named trees.

Scans README.md, docs/*.md (not docs/internal/), and a WHITEPAPER if present.
Does not assert CUDA acceleration is integrated.

Usage:
    python3 scripts/ci/check_doc_vs_code.py
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

# token -> required path (file or directory). None = src/ or include/.
TOKENS: tuple[tuple[str, str | None], ...] = (
    ("RETDEC_NEURAL_REFINE", "src/neural"),
    ("RETDEC_EMIT_BUILDABLE", "src/retdec"),
    ("RETDEC_SKIP_SEMANTIC_RECOVERY", "src/retdec/retdec.cpp"),
    ("maybeRefineDecompilerOutput", None),
)

SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def public_doc_files() -> list[Path]:
    files: list[Path] = []
    readme = REPO_ROOT / "README.md"
    if readme.is_file():
        files.append(readme)
    docs = REPO_ROOT / "docs"
    if docs.is_dir():
        for path in sorted(docs.iterdir()):
            if path.is_file() and path.suffix.lower() == ".md":
                files.append(path)
    for name in ("WHITEPAPER.md", "WHITEPAPER"):
        path = REPO_ROOT / name
        if path.is_file() and path not in files:
            files.append(path)
    return files


def symbol_present(token: str, rel: str | None) -> bool:
    if rel is None:
        roots = (REPO_ROOT / "src", REPO_ROOT / "include")
    else:
        roots = (REPO_ROOT / rel,)

    for root in roots:
        if root.is_file():
            return token in read_text(root)
        if not root.is_dir():
            continue
        try:
            children = root.rglob("*")
        except OSError:
            continue
        for path in children:
            if not path.is_file():
                continue
            if path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if token in read_text(path):
                return True
    return False


def docs_mentioning(token: str, docs: list[Path]) -> list[str]:
    hits = []
    for path in docs:
        if token in read_text(path):
            hits.append(path.relative_to(REPO_ROOT).as_posix())
    return hits


def main() -> int:
    docs = public_doc_files()
    errors: list[str] = []

    print("check_doc_vs_code: public docs scanned:")
    for path in docs:
        print(f"  {path.relative_to(REPO_ROOT).as_posix()}")

    for token, rel in TOKENS:
        where = rel or "src/ or include/"
        present = symbol_present(token, rel)
        advertised = docs_mentioning(token, docs)
        in_readme = "README.md" in advertised
        status = "present" if present else "MISSING"
        adv = f"; advertised in {', '.join(advertised)}" if advertised else ""
        print(f"  {token} -> {where}: {status}{adv}")

        if present:
            continue
        if in_readme:
            errors.append(
                f"{token} is advertised in README.md but the symbol is missing in {where}"
            )
        else:
            errors.append(f"{token} has no matching symbol in {where}")

    if errors:
        print("check_doc_vs_code: FAIL")
        for line in errors:
            print(f"  {line}")
        return 1

    print("check_doc_vs_code: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
