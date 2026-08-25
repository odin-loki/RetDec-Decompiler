#!/usr/bin/env python3
"""CI-04 — binaries named in public release-artefact tables must be executables.

A markdown table cell that names `retdec-foo` or `retdec-foo.exe` must match
an `add_executable` target under src/ (or that target's OUTPUT_NAME).
Package globs (tarballs, setup.exe templates) are ignored.

Usage:
    python3 scripts/ci/check_release_binaries.py
    python3 scripts/ci/check_release_binaries.py --self-test
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

ADD_EXEC_RE = re.compile(
    r"add_executable\(\s*([A-Za-z0-9_:-]+)",
    re.MULTILINE,
)
SET_PROPS_RE = re.compile(
    r"set_target_properties\(\s*([A-Za-z0-9_:-]+)\s+"
    r"PROPERTIES\b(.*?)\)",
    re.DOTALL,
)
OUTPUT_NAME_RE = re.compile(r'OUTPUT_NAME\s+"([^"]+)"')
CELL_RE = re.compile(
    r"\|\s*`?(retdec-[a-z0-9-]+)(?:\.exe)?`?\s*\|",
    re.IGNORECASE,
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def public_doc_files() -> list[Path]:
    files: list[Path] = []
    for rel in ("README.md", "releases/README.md", "QUICKSTART.md"):
        path = REPO_ROOT / rel
        if path.is_file():
            files.append(path)
    docs = REPO_ROOT / "docs"
    if docs.is_dir():
        for path in sorted(docs.iterdir()):
            if path.is_file() and path.suffix.lower() == ".md":
                files.append(path)
    return files


def src_cmake_files() -> list[Path]:
    src = REPO_ROOT / "src"
    if not src.is_dir():
        return []
    return [p for p in src.rglob("CMakeLists.txt") if p.is_file()]


def product_executables() -> set[str]:
    """add_executable target names plus OUTPUT_NAME of those targets."""
    exec_targets: set[str] = set()
    outputs: list[tuple[str, str]] = []
    for path in src_cmake_files():
        text = read_text(path)
        for m in ADD_EXEC_RE.finditer(text):
            exec_targets.add(m.group(1))
        for m in SET_PROPS_RE.finditer(text):
            target = m.group(1)
            out = OUTPUT_NAME_RE.search(m.group(2))
            if out:
                outputs.append((target, out.group(1)))
    names = set(exec_targets)
    for target, out_name in outputs:
        if target in exec_targets:
            names.add(out_name)
    return names


def skip_package_token(name: str) -> bool:
    if any(ch in name for ch in "*<>"):
        return True
    lowered = name.lower()
    if lowered.endswith((".tar", ".gz", ".xz", ".zip")):
        return True
    if "setup" in lowered or "portable" in lowered or "linux-x64" in lowered:
        return True
    if "windows-x64" in lowered:
        return True
    return False


def documented_release_binaries(docs: list[Path]) -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for doc in docs:
        rel = doc.relative_to(REPO_ROOT).as_posix()
        for m in CELL_RE.finditer(read_text(doc)):
            name = m.group(1)
            if skip_package_token(name):
                continue
            key = (rel, name)
            if key in seen:
                continue
            seen.add(key)
            found.append((rel, name))
    return found


def check() -> list[str]:
    errors: list[str] = []
    products = product_executables()
    docs = public_doc_files()
    named = documented_release_binaries(docs)
    if not named:
        errors.append("no retdec-* binaries found in public release-artefact tables")
        return errors
    for rel, name in named:
        if name not in products:
            errors.append(f"{rel}: `{name}` is not an add_executable / OUTPUT_NAME under src/")
    return errors


def self_test() -> int:
    products = product_executables()
    required = ("retdec-decompiler", "retdec-gui", "retdec-unpacker")
    missing = [n for n in required if n not in products]
    if missing:
        print("check_release_binaries --self-test: FAIL")
        for n in missing:
            print(f"  product executables missing {n}")
        return 1
    win = REPO_ROOT / "docs" / "WINDOWS_NATIVE_BUILD.md"
    named = {n for _, n in documented_release_binaries([win])}
    for n in required:
        if n not in named:
            print("check_release_binaries --self-test: FAIL")
            print(f"  WINDOWS_NATIVE_BUILD.md table missing {n}")
            return 1
    print("check_release_binaries --self-test: OK")
    return 0


def main() -> int:
    if "--self-test" in sys.argv:
        return self_test()
    errors = check()
    if errors:
        print("check_release_binaries: FAIL")
        for e in errors:
            print(f"  {e}")
        return 1
    named = documented_release_binaries(public_doc_files())
    uniq = sorted({n for _, n in named})
    print("check_release_binaries: OK")
    print("  binaries: " + ", ".join(uniq))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
