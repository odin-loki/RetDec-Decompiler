#!/usr/bin/env python3
"""E8 — first-level src/ modules must be referenced from outside themselves.

A module is referenced if its name appears in any CMakeLists.txt outside
that directory, in include/retdec, or another src/*/ file includes its
header (retdec/<module>/... or retdec/<module>.h).

Unintegrated experimental trees stay allowlisted (cuda_accel, opencl, and
any src/*/README.md that already says unintegrated). src/experimental/ is
never a failure.

Usage:
    python3 scripts/ci/check_link_graph.py
    python3 scripts/ci/check_link_graph.py --self-test
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SRC_ROOT = REPO_ROOT / "src"
INCLUDE_RETDEC = REPO_ROOT / "include" / "retdec"

# Documented unintegrated / not-in-pipeline trees (see src/*/README.md).
ALLOWLIST = frozenset({"cuda_accel", "opencl"})
EXEMPT_MODULES = frozenset({"experimental"})
SKIP_DIR_NAMES = frozenset(
    {"deps", "build", ".git", "corpus", "__pycache__", "node_modules"}
)
SOURCE_SUFFIXES = frozenset(
    {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc"}
)
INCLUDE_RE = re.compile(
    r'#\s*include\s*[<"]retdec/([^/">]+)(?:/[^>"]*|\.h(?:pp)?)[>"]'
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def iter_files(root: Path):
    if not root.is_dir():
        return
    stack = [root]
    while stack:
        current = stack.pop()
        try:
            children = list(current.iterdir())
        except OSError:
            continue
        for path in children:
            name = path.name
            if path.is_dir():
                if name in SKIP_DIR_NAMES or name.startswith("."):
                    continue
                stack.append(path)
            elif path.is_file():
                yield path


def dir_has_files(directory: Path) -> bool:
    try:
        for path in directory.iterdir():
            if path.is_file() or path.is_dir():
                return True
    except OSError:
        return False
    return False


def src_modules() -> list[str]:
    if not SRC_ROOT.is_dir():
        return []
    names = []
    try:
        children = list(SRC_ROOT.iterdir())
    except OSError:
        return []
    for path in children:
        if path.is_dir() and dir_has_files(path):
            names.append(path.name)
    return sorted(names)


def is_unintegrated_readme(name: str) -> bool:
    readme = SRC_ROOT / name / "README.md"
    if not readme.is_file():
        return False
    return "unintegrated" in readme.read_text(encoding="utf-8", errors="replace").lower()


def token_regex(names: list[str]) -> re.Pattern[str] | None:
    if not names:
        return None
    parts = [re.escape(n) for n in sorted(names, key=len, reverse=True)]
    return re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(parts) + r")(?![A-Za-z0-9_])")


def collect_referenced(names: list[str]) -> dict[str, list[str]]:
    referenced: dict[str, list[str]] = {n: [] for n in names}
    name_re = token_regex(names)
    if name_re is None:
        return referenced

    cmake_roots = (
        SRC_ROOT,
        INCLUDE_RETDEC,
        REPO_ROOT / "cmake",
        REPO_ROOT / "tests",
    )
    root_cmake = REPO_ROOT / "CMakeLists.txt"
    if root_cmake.is_file():
        rel = root_cmake.relative_to(REPO_ROOT).as_posix()
        text = read_text(root_cmake)
        for match in name_re.finditer(text):
            name = match.group(1)
            hits = referenced[name]
            if rel not in hits:
                hits.append(f"cmake:{rel}")

    for root in cmake_roots:
        for path in iter_files(root):
            if path.name != "CMakeLists.txt":
                continue
            rel = path.relative_to(REPO_ROOT).as_posix()
            text = read_text(path)
            for match in name_re.finditer(text):
                name = match.group(1)
                try:
                    path.relative_to(SRC_ROOT / name)
                except ValueError:
                    hits = referenced[name]
                    if rel not in hits:
                        hits.append(f"cmake:{rel}")

    if INCLUDE_RETDEC.is_dir():
        for path in iter_files(INCLUDE_RETDEC):
            rel = path.relative_to(REPO_ROOT).as_posix()
            blob = rel + "\n" + read_text(path)
            for match in name_re.finditer(blob):
                name = match.group(1)
                hits = referenced[name]
                if "include/retdec" not in hits:
                    hits.append("include/retdec")

    for path in iter_files(SRC_ROOT):
        if path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        text = read_text(path)
        for match in INCLUDE_RE.finditer(text):
            name = match.group(1)
            if name not in referenced:
                continue
            try:
                path.relative_to(SRC_ROOT / name)
            except ValueError:
                rel = path.relative_to(REPO_ROOT).as_posix()
                hits = referenced[name]
                key = f"include-from:{rel}"
                if key not in hits:
                    hits.append(key)

    return referenced


def analyze() -> tuple[set[str], list[str]]:
    names = src_modules()
    referenced_map = collect_referenced(names)
    referenced = {n for n, hits in referenced_map.items() if hits}
    orphans = [n for n in names if n not in referenced]
    return referenced, orphans


def failing_orphans(orphans: list[str]) -> list[str]:
    failing = []
    for name in orphans:
        if name in EXEMPT_MODULES:
            continue
        if name in ALLOWLIST or is_unintegrated_readme(name):
            continue
        failing.append(name)
    return failing


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="assert src/retdec is referenced, then run the orphan check",
    )
    args = parser.parse_args(argv)

    referenced, orphans = analyze()

    if args.self_test:
        if "retdec" not in referenced:
            print("check_link_graph --self-test: FAIL (src/retdec not referenced)")
            return 1
        print("check_link_graph --self-test: OK (src/retdec is referenced)")

    if orphans:
        print("check_link_graph: unreferenced src/ modules:")
        for name in orphans:
            tags = []
            if name in EXEMPT_MODULES:
                tags.append("experimental")
            elif name in ALLOWLIST or is_unintegrated_readme(name):
                tags.append("allowlisted")
            suffix = f" ({', '.join(tags)})" if tags else ""
            print(f"  {name}{suffix}")
    else:
        print("check_link_graph: no unreferenced src/ modules")

    failing = failing_orphans(orphans)
    if failing:
        print("check_link_graph: FAIL — orphans not allowlisted and not experimental:")
        for name in failing:
            print(f"  {name}")
        return 1

    print("check_link_graph: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
