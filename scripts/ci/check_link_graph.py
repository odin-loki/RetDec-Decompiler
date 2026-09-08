#!/usr/bin/env python3
"""E8 — first-level src/ modules must be referenced from outside themselves,
and the ones the product does not link must be the ones we say.

A module is referenced if its name appears in any CMakeLists.txt outside
that directory, in include/retdec, or another src/*/ file includes its
header (retdec/<module>/... or retdec/<module>.h).

Unintegrated experimental trees stay allowlisted (cuda_accel, opencl, and
any src/*/README.md that already says unintegrated). src/experimental/ is
never a failure.

The second question is sharper than the first and nothing asked it: a
library whose only consumer is its own unit-test binary passes the
reference check -- tests/<module>/CMakeLists.txt names it -- while shipping
in nothing. Twelve libraries under src/ are in that state, and no source
outside their own directory includes their headers either, so a fix landing
in one of them reaches no user of the decompiler. That is not a defect to
repair by deleting them or by wiring twelve subsystems into the pipeline;
it is a fact about the tree that should be written down and counted, so it
cannot grow by one more without somebody saying so.

TEST_ONLY_LIBRARIES below is that list, with a reason each. Adding a library
that nothing links, or linking one of these into the product, fails this
check until the list is updated to match.

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

# Library targets under src/ whose only consumer is a test binary. Each is
# built, unit-tested and installed, and reaches no decompilation: nothing
# under src/ or the root links it, and no source outside its own directory
# includes its headers. Measured, not assumed -- test_only_libraries() below
# recomputes it from the CMakeLists.
TEST_ONLY_LIBRARIES = {
    "retdec-cfg": "CFG reconstruction; the pipeline uses bin2llvmir's own CFG",
    "retdec-code-data": "code/data separation; the loader decides that today",
    "retdec-compiler-abi": "ABI tables; param_return in bin2llvmir carries its own",
    "retdec-compiler-detect": "compiler identification; cpdetect is what runs",
    "retdec-eh-reconstruct": "exception-handler recovery, not yet consumed by any emitter",
    "retdec-func-boundary": "function boundary detection; the decoder finds functions itself",
    "retdec-idiom-reconstruct": "idiom recovery; llvmir2hll has its own idiom passes",
    "retdec-loader-sim": "loader simulation, used only by its own tests",
    "retdec-module-cluster": "module clustering, no caller",
    "retdec-rtti": "RTTI reconstruction; rtti-finder is the one bin2llvmir links",
    "retdec-string-detect": "string classification, no caller",
    "retdec-testing": "test support library, which is what it is for",
}

# Library targets that nothing links at all, including tests.
UNLINKED_LIBRARIES = {
    "retdec-experimental": "task scaffold behind RETDEC_ENABLE_EXPERIMENTAL_SCAFFOLD",
}

CMAKE_COMMENT_RE = re.compile(r"#[^\n]*")
ADD_LIBRARY_RE = re.compile(r"add_library\(\s*([A-Za-z0-9_.:-]+)")
ALIAS_RE = re.compile(
    r"add_library\(\s*([A-Za-z0-9_.:-]+)\s+ALIAS\s+([A-Za-z0-9_.:-]+)\s*\)"
)
LINK_RE = re.compile(r"target_link_libraries\(([^)]*)\)", re.S)
CMAKE_TOKEN_RE = re.compile(r"[A-Za-z0-9_.:-]+")


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


def _cmake_files() -> list[Path]:
    """Every CMakeLists.txt that can name a library target."""
    files = []
    root_cmake = REPO_ROOT / "CMakeLists.txt"
    if root_cmake.is_file():
        files.append(root_cmake)
    for root in (SRC_ROOT, REPO_ROOT / "tests"):
        for path in iter_files(root):
            if path.name == "CMakeLists.txt":
                files.append(path)
    return files


def test_only_libraries() -> tuple[dict[str, str], dict[str, str]]:
    """Library targets defined under src/, split by who links them.

    Returns (linked only by tests/, linked by nothing at all), each mapping
    the target name to the directory that defines it.

    Comments are stripped before the call bodies are read: a `)` inside one --
    src/retdec/CMakeLists.txt has "(PUBLIC: symbols used in retdec static lib)"
    -- ends the argument list early and silently hides every entry after it.
    """
    definitions: dict[str, str] = {}
    aliases: dict[str, str] = {}
    linked_by_product: set[str] = set()
    linked_by_tests: set[str] = set()

    for path in _cmake_files():
        text = CMAKE_COMMENT_RE.sub("", read_text(path))
        rel = path.relative_to(REPO_ROOT).as_posix()
        is_test = rel.startswith("tests/")

        if not is_test and rel != "CMakeLists.txt":
            for match in ALIAS_RE.finditer(text):
                aliases[match.group(1)] = match.group(2)
            for match in ADD_LIBRARY_RE.finditer(text):
                name = match.group(1)
                if not name.startswith("$"):
                    definitions.setdefault(name, path.parent.relative_to(REPO_ROOT).as_posix())

        target = linked_by_tests if is_test else linked_by_product
        for match in LINK_RE.finditer(text):
            target.update(CMAKE_TOKEN_RE.findall(match.group(1)))

    def names_of(target: str) -> set[str]:
        return {target} | {a for a, real in aliases.items() if real == target}

    only_tests: dict[str, str] = {}
    unlinked: dict[str, str] = {}
    for name, where in sorted(definitions.items()):
        if name in aliases:
            continue
        if names_of(name) & linked_by_product:
            continue
        if names_of(name) & linked_by_tests:
            only_tests[name] = where
        else:
            unlinked[name] = where
    return only_tests, unlinked


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

    only_tests, unlinked = test_only_libraries()
    drift = []
    for name, where in sorted(only_tests.items()):
        if name not in TEST_ONLY_LIBRARIES:
            drift.append(f"{name} ({where}) is linked only by its tests and is not in TEST_ONLY_LIBRARIES")
    for name in sorted(TEST_ONLY_LIBRARIES):
        if name not in only_tests:
            drift.append(f"{name} is in TEST_ONLY_LIBRARIES but is no longer linked only by its tests")
    for name, where in sorted(unlinked.items()):
        if name not in UNLINKED_LIBRARIES:
            drift.append(f"{name} ({where}) is linked by nothing at all and is not in UNLINKED_LIBRARIES")
    for name in sorted(UNLINKED_LIBRARIES):
        if name not in unlinked:
            drift.append(f"{name} is in UNLINKED_LIBRARIES but something links it now")

    if drift:
        print("check_link_graph: FAIL — the set of libraries the product does not link has changed:")
        for line in drift:
            print(f"  {line}")
        print("       update TEST_ONLY_LIBRARIES / UNLINKED_LIBRARIES in this script to match, "
              "with a reason")
        return 1

    print(f"check_link_graph: {len(only_tests)} librar(y/ies) linked only by their tests, "
          f"{len(unlinked)} by nothing, both as recorded")
    print("check_link_graph: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
