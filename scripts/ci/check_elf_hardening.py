#!/usr/bin/env python3
"""QUAL-08 — RELRO/PIE/NX/canary-style ELF hardening on shipped Linux binaries.

Uses readelf from binutils. Does not require the checksec package.

Hard-fails when a shipped ELF executable is present but lacks PIE, NX, or
at least Partial RELRO (GNU_RELRO). Canary (__stack_chk_fail) is reported;
it is not a hard fail so Debug trees without -fstack-protector still pass.

Skip-safe: if DIR is missing or contains no shipped ELF executables, print
a clear SKIP message and exit 0 (configure-only / empty trees).

Usage:
    python3 scripts/ci/check_elf_hardening.py DIR
    python3 scripts/ci/check_elf_hardening.py --self-test
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
SKIP_DIR_NAMES = frozenset(
    {
        ".git",
        ".venv",
        "CMakeFiles",
        "__pycache__",
        "deps",
        "external",
        "llvm",
        "node_modules",
        "tests",
        "Testing",
    }
)
SKIP_NAME_RE = re.compile(
    r"(test|fixture|probe|staging|smoke|corpus)",
    re.IGNORECASE,
)
TYPE_RE = re.compile(r"^\s*Type:\s+(\w+)", re.MULTILINE)
GNU_STACK_RE = re.compile(
    r"GNU_STACK\b[^\n]*(?:\n[ \t]+[^\n]*)?",
    re.MULTILINE,
)
STACK_FLAGS_RE = re.compile(r"\b([RWE]{1,3})\b")
BIND_NOW_RE = re.compile(
    r"\((?:BIND_NOW|FLAGS|FLAGS_1)\)[^\n]*\b(?:BIND_NOW|NOW)\b"
)


def is_elf_file(path: Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == ELF_MAGIC
    except OSError:
        return False


def is_shipped_name(name: str) -> bool:
    if name.endswith(".so") or ".so." in name:
        return False
    if name == "retdec" or name.startswith("retdec-"):
        return SKIP_NAME_RE.search(name) is None
    return False


def collect_targets(root: Path) -> list[Path]:
    if root.is_file():
        return [root] if is_elf_file(root) else []
    if not root.is_dir():
        return []
    found: list[Path] = []
    seen: set[Path] = set()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIR_NAMES]
        for name in filenames:
            path = Path(dirpath) / name
            if not is_shipped_name(name) or not is_elf_file(path):
                continue
            try:
                key = path.resolve()
            except OSError:
                key = path
            if key in seen:
                continue
            seen.add(key)
            found.append(path)
    return sorted(found)


def run_readelf(readelf: str, args: list[str], path: Path) -> str:
    proc = subprocess.run(
        [readelf, *args, str(path)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
        check=False,
    )
    if proc.returncode != 0:
        err = (proc.stderr or proc.stdout or "").strip() or f"exit {proc.returncode}"
        raise RuntimeError(f"readelf {' '.join(args)} {path}: {err}")
    return proc.stdout


def parse_elf_type(header_text: str) -> str:
    match = TYPE_RE.search(header_text)
    return match.group(1) if match else ""


def is_elf_executable(header_text: str, phdr_text: str) -> bool:
    elf_type = parse_elf_type(header_text)
    if elf_type == "EXEC":
        return True
    if elf_type == "DYN" and re.search(r"\bINTERP\b", phdr_text):
        return True
    return False


def has_pie(header_text: str) -> bool:
    if "Position-Independent Executable" in header_text:
        return True
    return parse_elf_type(header_text) == "DYN"


def has_nx(phdr_text: str) -> bool:
    match = GNU_STACK_RE.search(phdr_text)
    if not match:
        return False
    flags = STACK_FLAGS_RE.findall(match.group(0))
    if not flags:
        return False
    return "E" not in flags[-1]


def relro_status(phdr_text: str, dynamic_text: str) -> str:
    if "GNU_RELRO" not in phdr_text:
        return "none"
    if BIND_NOW_RE.search(dynamic_text) or "(BIND_NOW)" in dynamic_text:
        return "full"
    return "partial"


def has_canary(dynsym_text: str) -> bool:
    return "__stack_chk_fail" in dynsym_text


def inspect_binary(readelf: str, path: Path) -> dict[str, object]:
    header = run_readelf(readelf, ["-h"], path)
    phdr = run_readelf(readelf, ["-l", "-W"], path)
    if not is_elf_executable(header, phdr):
        return {"skipped": True}
    dynamic = run_readelf(readelf, ["-d", "-W"], path)
    dynsym = run_readelf(readelf, ["--dyn-syms", "-W"], path)
    pie = has_pie(header)
    nx = has_nx(phdr)
    relro = relro_status(phdr, dynamic)
    canary = has_canary(dynsym)
    errors: list[str] = []
    if not pie:
        errors.append("missing PIE (ELF type is not DYN)")
    if not nx:
        errors.append("missing NX (GNU_STACK is executable or absent)")
    if relro == "none":
        errors.append("missing RELRO (no GNU_RELRO)")
    return {
        "skipped": False,
        "pie": pie,
        "nx": nx,
        "relro": relro,
        "canary": canary,
        "errors": errors,
    }


def find_readelf() -> str | None:
    return shutil.which("readelf")


def run_check(root: Path) -> int:
    if not root.exists():
        print(f"check_elf_hardening: SKIP (directory does not exist: {root})")
        return 0
    targets = collect_targets(root)
    if not targets:
        print(f"check_elf_hardening: SKIP (no ELF executables in {root})")
        return 0
    readelf = find_readelf()
    if not readelf:
        print("check_elf_hardening: FAIL (readelf not found; install binutils)")
        return 1
    errors: list[str] = []
    checked = 0
    for path in targets:
        try:
            result = inspect_binary(readelf, path)
        except RuntimeError as exc:
            print("check_elf_hardening: FAIL")
            print(f"  {exc}")
            return 1
        if result.get("skipped"):
            continue
        checked += 1
        canary = "yes" if result["canary"] else "no"
        print(
            f"check_elf_hardening: {path} "
            f"PIE={'yes' if result['pie'] else 'no'} "
            f"NX={'yes' if result['nx'] else 'no'} "
            f"RELRO={result['relro']} "
            f"canary={canary}"
        )
        for item in result["errors"]:  # type: ignore[union-attr]
            errors.append(f"{path}: {item}")
    if checked == 0:
        print(f"check_elf_hardening: SKIP (no ELF executables in {root})")
        return 0
    if errors:
        print("check_elf_hardening: FAIL")
        for item in errors:
            print(f"  {item}")
        return 1
    print("check_elf_hardening: OK")
    return 0


def self_test() -> int:
    header_pie = "  Type:                              DYN (Shared object file)\n"
    header_exec = "  Type:                              EXEC (Executable file)\n"
    phdr_ok = (
        "  INTERP         0x0000000000000318 0x0000000000000318 0x0000000000000318\n"
        "  GNU_STACK      0x0000000000000000 0x0000000000000000 0x0000000000000000\n"
        "                 0x0000000000000000 0x0000000000000000  RW     0x10\n"
        "  GNU_RELRO      0x00000000002de000 0x00000000002de000 0x00000000002de000\n"
    )
    phdr_rwe = (
        "  INTERP         0x0000000000000318 0x0000000000000318 0x0000000000000318\n"
        "  GNU_STACK      0x0000000000000000 0x0000000000000000 0x0000000000000000\n"
        "                 0x0000000000000000 0x0000000000000000  RWE    0x10\n"
    )
    dyn_full = " 0x000000000000001e (FLAGS)              BIND_NOW\n"
    dyn_none = " 0x0000000000000003 (PLTGOT)             0x2e0000\n"
    dynsym = "     2: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND __stack_chk_fail\n"
    errors: list[str] = []
    if not has_pie(header_pie) or has_pie(header_exec):
        errors.append("PIE parse")
    if not has_nx(phdr_ok) or has_nx(phdr_rwe) or has_nx(""):
        errors.append("NX parse")
    if relro_status(phdr_ok, dyn_none) != "partial":
        errors.append("RELRO partial")
    if relro_status(phdr_ok, dyn_full) != "full":
        errors.append("RELRO full")
    if relro_status(phdr_rwe, dyn_none) != "none":
        errors.append("RELRO none")
    if not has_canary(dynsym) or has_canary(""):
        errors.append("canary parse")
    if not is_elf_executable(header_pie, phdr_ok):
        errors.append("executable DYN+INTERP")
    if is_elf_executable(header_pie, "  GNU_STACK  RW\n"):
        errors.append("shared object should not count as executable")
    if is_shipped_name("retdec-decompiler") is False:
        errors.append("shipped name")
    if is_shipped_name("retdec-gui-tests") or is_shipped_name("libretdec-utils.so"):
        errors.append("skip test/library names")
    if errors:
        print("check_elf_hardening --self-test: FAIL")
        for item in errors:
            print(f"  {item}")
        return 1
    print("check_elf_hardening --self-test: OK")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        help="Build or install tree that may contain shipped ELF executables",
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.root is None:
        print("check_elf_hardening: FAIL (pass a directory or --self-test)")
        return 1
    return run_check(args.root)


if __name__ == "__main__":
    sys.exit(main())
