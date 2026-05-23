#!/usr/bin/env python3
"""
Managed-format decompiler smoke test.

For each managed input fixture under tests/, run retdec-decompiler and verify
non-empty decompiled output. Skips formats whose fixture file is missing.

Usage:
    python3 managed_format_smoke_test.py <decompiler_path> [repo_root]
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional, Tuple


@dataclass
class ManagedCase:
    name: str
    fixture: Path
    output_ext: str
    prepare: Optional[Callable[[Path, Path], bool]] = None


def repo_root_from_argv() -> Path:
    if len(sys.argv) >= 3:
        return Path(sys.argv[2]).resolve()
    return Path(__file__).resolve().parents[2]


def compile_python_pyc(src: Path, dest: Path) -> bool:
    if not src.is_file():
        return False
    import py_compile

    try:
        py_compile.compile(str(src), cfile=str(dest), doraise=True)
        return dest.is_file() and dest.stat().st_size > 0
    except py_compile.PyCompileError:
        return False


def compile_java_class(src: Path, dest_dir: Path) -> Optional[Path]:
    if not src.is_file():
        return None
    import shutil

    javac = shutil.which("javac")
    if not javac:
        return None
    try:
        subprocess.run(
            [javac, "-d", str(dest_dir), str(src)],
            check=True,
            capture_output=True,
            timeout=60,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    out = dest_dir / (src.stem + ".class")
    return out if out.is_file() and out.stat().st_size > 0 else None


def compile_lua_luac(src: Path, dest: Path) -> bool:
    if not src.is_file():
        return False
    import shutil

    luac = shutil.which("luac")
    if not luac:
        return False
    try:
        subprocess.run(
            [luac, "-o", str(dest), str(src)],
            check=True,
            capture_output=True,
            timeout=30,
        )
        return dest.is_file() and dest.stat().st_size > 0
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return False


def write_minimal_wasm(dest: Path) -> bool:
    # Valid WASM header (version 1). Parser may accept an empty module body.
    dest.write_bytes(b"\x00asm\x01\x00\x00\x00")
    return dest.stat().st_size > 0


def prepare_java_fixture(fixture: Path, work: Path, src: Path) -> bool:
    compiled = compile_java_class(src, work)
    if not compiled:
        return False
    import shutil

    fixture.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(compiled, fixture)
    return fixture.is_file() and fixture.stat().st_size > 0


def build_cases(root: Path) -> List[ManagedCase]:
    mi = root / "tests" / "managed_integration" / "fixtures"
    dec = root / "tests" / "decompiler" / "fixtures"
    java_src = mi / "java" / "Hello.java"
    py_src = mi / "python" / "hello.py"
    lua_src = mi / "lua" / "hello.lua"
    return [
        ManagedCase(
            "java_class",
            mi / "java" / "Hello.class",
            ".java",
            lambda f, w: prepare_java_fixture(f, w, java_src),
        ),
        ManagedCase(
            "python_pyc",
            mi / "python" / "hello.pyc",
            ".py",
            lambda f, w: compile_python_pyc(py_src, f),
        ),
        ManagedCase(
            "lua_luac",
            mi / "lua" / "hello.luac",
            ".lua",
            lambda f, w: compile_lua_luac(lua_src, f),
        ),
        ManagedCase(
            "wasm",
            dec / "minimal.wasm",
            ".wat",
            lambda f, w: write_minimal_wasm(f),
        ),
    ]


def run_case(decompiler: Path, case: ManagedCase, work: Path) -> Tuple[str, bool, str]:
    fixture = case.fixture
    if case.prepare and not fixture.is_file():
        fixture.parent.mkdir(parents=True, exist_ok=True)
        if not case.prepare(fixture, work):
            return case.name, False, "SKIP (fixture missing, compile failed)"

    if not fixture.is_file():
        return case.name, False, "SKIP (fixture missing)"

    out_base = work / case.name
    out_path = str(out_base) + case.output_ext
    try:
        proc = subprocess.run(
            [str(decompiler), "-o", out_path, str(fixture)],
            capture_output=True,
            text=True,
            timeout=120,
        )
    except subprocess.TimeoutExpired:
        return case.name, False, "FAIL (timeout)"

    if proc.returncode != 0:
        tail = (proc.stderr or proc.stdout or "")[-1500:]
        return case.name, False, f"FAIL (exit {proc.returncode})\n{tail}"

    # WASM emitter writes alongside requested path with .wat extension.
    candidates = [Path(out_path)]
    if case.output_ext == ".wat":
        candidates.append(Path(str(out_base) + ".wat"))

    for p in candidates:
        if p.is_file() and p.stat().st_size > 0:
            return case.name, True, f"OK ({p.name}, {p.stat().st_size} bytes)"

    return case.name, False, "FAIL (output empty or missing)"


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: managed_format_smoke_test.py <decompiler> [repo_root]", file=sys.stderr)
        return 1

    decompiler = Path(sys.argv[1])
    if not decompiler.is_file():
        print(f"Error: decompiler not found: {decompiler}", file=sys.stderr)
        return 1

    root = repo_root_from_argv()
    cases = build_cases(root)

    passed = skipped = failed = 0
    with tempfile.TemporaryDirectory(prefix="retdec_managed_smoke_") as tmp:
        work = Path(tmp)
        for case in cases:
            name, ok, msg = run_case(decompiler, case, work)
            print(f"[{name}] {msg}")
            if msg.startswith("SKIP"):
                skipped += 1
            elif ok:
                passed += 1
            else:
                failed += 1

    print(f"\nSummary: {passed} passed, {skipped} skipped, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
