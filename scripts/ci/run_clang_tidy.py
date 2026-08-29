#!/usr/bin/env python3
"""QUAL-01 — clang-tidy on Imortek-new modules (src/neural, src/gui,
src/codegen, src/ssa, src/algo_recover, src/cli_parser,
src/crypto_detect, src/sort_detect, src/jvm_parser, src/pyc_parser,
src/dex_parser, src/ptx_decompile).

Skip-safe if clang-tidy is missing (print SKIP, exit 0). Uses
build/linux/compile_commands.json when present; otherwise runs --self-test.
Only implementation files under those Imortek dirs are listed.

First land is warn-only: tidy findings are printed and do not fail CI.
The job fails only on script errors or a failed --self-test.

Usage:
    python3 scripts/ci/run_clang_tidy.py
    python3 scripts/ci/run_clang_tidy.py --self-test
    python3 scripts/ci/run_clang_tidy.py --compile-commands PATH
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
CLANG_TIDY_CONFIG = REPO_ROOT / ".clang-tidy"
DEFAULT_COMPILE_DB = REPO_ROOT / "build" / "linux" / "compile_commands.json"
IMORTEK_REL_DIRS = (
    "src/neural",
    "src/gui",
    "src/codegen",
    "src/ssa",
    "src/algo_recover",
    "src/cli_parser",
    "src/crypto_detect",
    "src/sort_detect",
    "src/jvm_parser",
    "src/pyc_parser",
    "src/dex_parser",
    "src/ptx_decompile",
)
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".cxx"})
SKIP_DIR_NAMES = frozenset(
    {".git", "deps", "build", "CMakeFiles", "__pycache__", "node_modules"}
)


def posix_rel(path: Path, root: Path = REPO_ROOT) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix().replace("\\", "/")


def is_imortek_rel(rel_posix: str) -> bool:
    return any(
        rel_posix == prefix or rel_posix.startswith(prefix + "/")
        for prefix in IMORTEK_REL_DIRS
    )


def list_imortek_sources(root: Path = REPO_ROOT) -> list[Path]:
    found: list[Path] = []
    for rel in IMORTEK_REL_DIRS:
        directory = root / Path(*rel.split("/"))
        if not directory.is_dir():
            continue
        stack = [directory]
        while stack:
            current = stack.pop()
            try:
                children = list(current.iterdir())
            except OSError:
                continue
            for path in children:
                if path.is_dir():
                    if path.name in SKIP_DIR_NAMES or path.name.startswith("."):
                        continue
                    stack.append(path)
                elif path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                    if is_imortek_rel(posix_rel(path, root)):
                        found.append(path)
    found.sort(key=lambda p: posix_rel(p, root))
    return found


def load_compile_db_files(db_path: Path) -> set[Path]:
    try:
        data = json.loads(db_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"invalid compile_commands.json: {exc}") from exc
    if not isinstance(data, list):
        raise RuntimeError("compile_commands.json is not a list")
    files: set[Path] = set()
    for entry in data:
        if not isinstance(entry, dict):
            continue
        raw = entry.get("file")
        if not raw or not isinstance(raw, str):
            continue
        path = Path(raw)
        if not path.is_absolute():
            directory = Path(entry.get("directory") or db_path.parent)
            path = directory / path
        try:
            files.add(path.resolve())
        except OSError:
            files.add(path)
    return files


def sources_in_compile_db(sources: list[Path], db_files: set[Path]) -> list[Path]:
    resolved_db = set(db_files)
    matched: list[Path] = []
    for path in sources:
        try:
            resolved = path.resolve()
        except OSError:
            resolved = path
        if resolved in resolved_db:
            matched.append(path)
    return matched


def find_clang_tidy() -> str | None:
    return shutil.which("clang-tidy")


def read_clang_tidy_config(path: Path = CLANG_TIDY_CONFIG) -> str:
    return path.read_text(encoding="utf-8")


def self_test() -> int:
    errors: list[str] = []
    if not CLANG_TIDY_CONFIG.is_file():
        errors.append("missing .clang-tidy")
        print("run_clang_tidy --self-test: FAIL")
        for item in errors:
            print(f"  {item}")
        return 1
    config = read_clang_tidy_config()
    if "-*" not in config:
        errors.append(".clang-tidy must disable all checks before the allowlist")
    if "bugprone-" not in config:
        errors.append(".clang-tidy must allow bugprone-")
    if "cert-" not in config:
        errors.append(".clang-tidy must allow cert-")
    if (
        "retdec/(neural|gui|codegen|ssa|algo_recover|cli_parser|crypto_detect|"
        "sort_detect|jvm_parser|pyc_parser|dex_parser|ptx_decompile)/"
        not in config.replace(" ", "")
    ):
        errors.append(
            ".clang-tidy HeaderFilterRegex must limit to retdec/neural, "
            "retdec/gui, retdec/codegen, retdec/ssa, retdec/algo_recover, "
            "retdec/cli_parser, retdec/crypto_detect, retdec/sort_detect, "
            "retdec/jvm_parser, retdec/pyc_parser, retdec/dex_parser, and "
            "retdec/ptx_decompile"
        )
    if "deps/llvm" in config or "bin2llvmir" in config:
        errors.append(".clang-tidy must not include LLVM/Avast header filters")

    sources = list_imortek_sources()
    rels = [posix_rel(p) for p in sources]
    if not any(r.startswith("src/neural/") for r in rels):
        errors.append("list_imortek_sources found no src/neural files")
    if not any(r.startswith("src/gui/") for r in rels):
        errors.append("list_imortek_sources found no src/gui files")
    if not any(r.startswith("src/codegen/") for r in rels):
        errors.append("list_imortek_sources found no src/codegen files")
    if not any(r.startswith("src/ssa/") for r in rels):
        errors.append("list_imortek_sources found no src/ssa files")
    if not any(r.startswith("src/algo_recover/") for r in rels):
        errors.append("list_imortek_sources found no src/algo_recover files")
    if not any(r.startswith("src/cli_parser/") for r in rels):
        errors.append("list_imortek_sources found no src/cli_parser files")
    if not any(r.startswith("src/crypto_detect/") for r in rels):
        errors.append("list_imortek_sources found no src/crypto_detect files")
    if not any(r.startswith("src/sort_detect/") for r in rels):
        errors.append("list_imortek_sources found no src/sort_detect files")
    if not any(r.startswith("src/jvm_parser/") for r in rels):
        errors.append("list_imortek_sources found no src/jvm_parser files")
    if not any(r.startswith("src/pyc_parser/") for r in rels):
        errors.append("list_imortek_sources found no src/pyc_parser files")
    if not any(r.startswith("src/dex_parser/") for r in rels):
        errors.append("list_imortek_sources found no src/dex_parser files")
    if not any(r.startswith("src/ptx_decompile/") for r in rels):
        errors.append("list_imortek_sources found no src/ptx_decompile files")
    leaked = [r for r in rels if not is_imortek_rel(r)]
    if leaked:
        errors.append(f"list leaked non-Imortek paths: {leaked[:3]}")
    if any("src/bin2llvmir" in r or "src/llvmir2hll" in r or "deps/llvm" in r for r in rels):
        errors.append("list included Avast/LLVM tree files")

    fake_db = {
        (REPO_ROOT / "src" / "neural" / "gates.cpp").resolve(),
        (REPO_ROOT / "src" / "bin2llvmir" / "not-imortek.cpp").resolve(),
    }
    matched = sources_in_compile_db(sources, fake_db)
    matched_rels = [posix_rel(p) for p in matched]
    if "src/neural/gates.cpp" not in matched_rels:
        errors.append("compile_commands filter missed src/neural/gates.cpp")
    if any("bin2llvmir" in r for r in matched_rels):
        errors.append("compile_commands filter kept Avast path")

    if (
        not is_imortek_rel("src/neural/gates.cpp")
        or not is_imortek_rel("src/gui/main.cpp")
        or not is_imortek_rel("src/codegen/emitter.cpp")
    ):
        errors.append("is_imortek_rel rejected valid paths")
    if is_imortek_rel("src/bin2llvmir/foo.cpp") or is_imortek_rel("src/gui_extra/x.cpp"):
        errors.append("is_imortek_rel accepted a non-Imortek path")

    if errors:
        print("run_clang_tidy --self-test: FAIL")
        for item in errors:
            print(f"  {item}")
        return 1
    print(
        f"run_clang_tidy --self-test: OK "
        f"({len(sources)} Imortek sources listed; clang-tidy not required)"
    )
    return 0


def run_tidy(compile_db: Path) -> int:
    tidy = find_clang_tidy()
    if not tidy:
        print("run_clang_tidy: SKIP (clang-tidy not found)")
        return 0
    if not CLANG_TIDY_CONFIG.is_file():
        print("run_clang_tidy: FAIL (missing .clang-tidy)")
        return 1
    try:
        db_files = load_compile_db_files(compile_db)
    except RuntimeError as exc:
        print(f"run_clang_tidy: FAIL ({exc})")
        return 1
    sources = list_imortek_sources()
    targets = sources_in_compile_db(sources, db_files)
    skipped = len(sources) - len(targets)
    if not targets:
        print(
            "run_clang_tidy: SKIP (no Imortek sources in compile_commands.json)"
        )
        return 0
    print(
        f"run_clang_tidy: {len(targets)} file(s) in compile db "
        f"({skipped} listed Imortek files not compiled; warn-only)"
    )
    cmd = [
        tidy,
        f"-p={compile_db.parent}",
        f"--config-file={CLANG_TIDY_CONFIG}",
        "--quiet",
        *[str(p) for p in targets],
    ]
    try:
        result = subprocess.run(cmd, cwd=REPO_ROOT, check=False)
    except OSError as exc:
        print(f"run_clang_tidy: FAIL (could not invoke clang-tidy: {exc})")
        return 1
    if result.returncode not in (0, 1):
        print(f"run_clang_tidy: FAIL (clang-tidy exited {result.returncode})")
        return 1
    print(
        "run_clang_tidy: WARN-ONLY (findings above do not fail CI; "
        "script errors still fail)"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument(
        "--compile-commands",
        type=Path,
        help="compile_commands.json (default: build/linux/compile_commands.json)",
    )
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if find_clang_tidy() is None:
        print("run_clang_tidy: SKIP (clang-tidy not found)")
        return 0
    compile_db = args.compile_commands or DEFAULT_COMPILE_DB
    if not compile_db.is_file():
        print(
            f"run_clang_tidy: no compile_commands.json at {compile_db}; "
            "running --self-test"
        )
        return self_test()
    return run_tidy(compile_db)


if __name__ == "__main__":
    sys.exit(main())
