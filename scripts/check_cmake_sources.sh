#!/usr/bin/env bash
# check_cmake_sources.sh - every source a CMakeLists names must exist.
#
# CMake fails a configure hard when add_library() or add_executable() names a
# file that is not there:
#
#   CMake Error: Cannot find source file: var_name_gen/var_name_gens/word_var_name_gen.cpp
#
# That is not a build failure this repository notices quickly. The targets that
# would catch it -- llvmir2hll, bin2llvmir, fileformat -- are behind the pinned
# LLVM build, which takes hours and only runs on main and on pull requests, so a
# deleted or renamed .cpp can sit in a source list for the whole life of a
# branch. This is a file-existence check over the source lists themselves: it
# needs no compiler, no dependencies and no configure, and it runs in a second.
#
# One source list entry is legitimately absent from a fresh clone:
# src/llvmir2hll/var_name_gen/var_name_gens/word_var_name_gen.cpp is a 50k-entry
# generated word list, gitignored and downloaded by scripts/fetch-large-files.sh
# like the rest of the large support data. So the manifest in that script is read
# here and a file it provides is reported as un-fetched rather than as missing --
# the source list is satisfiable, which is what this check is about. Delete such
# an entry from the manifest and it becomes a hard failure like any other.
#
# The second pass is the same question the other way round: a test source on
# disk that no CMakeLists.txt names is a file nobody runs. That is how ten of
# the twelve suites in tests/fileformat/ came to be dead, and how
# tests/utils/dynamic_buffer_tests.cpp sat unbuilt with ten regression tests in
# it. Files that are deliberately not listed carry a reason in UNBUILT below.
#
# Usage:
#   bash scripts/check_cmake_sources.sh          # whole tree
#   bash scripts/check_cmake_sources.sh src/foo  # one subtree
#
# Exit status is 0 when every named source either exists or is fetchable, 1
# otherwise.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

exec python3 - "$@" <<'PY'
import os
import re
import sys

ROOTS = sys.argv[1:] or ["."]

# The commands whose argument list is a list of source files. target_sources is
# included; its first argument is a target name and its keywords are filtered
# out with everything else that is not a path.
COMMANDS = ("add_library", "add_executable", "target_sources")

# Keywords that appear inside those calls and are not files.
KEYWORDS = {
    "STATIC", "SHARED", "MODULE", "OBJECT", "INTERFACE", "UNKNOWN",
    "IMPORTED", "GLOBAL", "ALIAS", "EXCLUDE_FROM_ALL", "WIN32", "MACOSX_BUNDLE",
    "PUBLIC", "PRIVATE", "FILE_SET", "TYPE", "BASE_DIRS", "FILES", "HEADERS",
}

SOURCE_SUFFIXES = (".cpp", ".cxx", ".cc", ".c", ".cu", ".m", ".mm", ".S", ".asm")

def fetchable_paths():
    """Repository-relative paths scripts/fetch-large-files.sh downloads."""
    try:
        with open("scripts/fetch-large-files.sh", encoding="utf-8") as fh:
            text = fh.read()
    except OSError:
        return set()
    block = re.search(r"^files=\((.*?)^\)", text, re.S | re.M)
    if not block:
        return set()
    return set(re.findall(r'"([^"]+)"', block.group(1)))

FETCHABLE = fetchable_paths()

# Test sources that no CMakeLists.txt names, on purpose. The value is the
# reason, printed with the file, so removing one from this list is a decision
# somebody has to write down rather than a silent deletion.
# Whole directories under tests/ that CMake deliberately does not build, with
# the reason and what does build them.
UNBUILT_DIRS = {
    "tests/verification":
        "ESBMC proof harnesses; compiled and discharged by "
        "scripts/verify_esbmc.sh, which is a solver run rather than a test "
        "binary -- see .github/workflows/verify-esbmc.yml",
    "tests/standalone":
        "the GoogleTest shim and its runner; compiled by "
        "scripts/standalone_check.sh, and building them under CMake would "
        "collide with the real GoogleTest the CMake suites link",
    "tests/benchmark":
        "decompiler INPUT, not test sources; tests/benchmark/benchmark_cpp.cpp "
        "is a program the benchmark scripts compile and then decompile",
    "tests/opencl":
        "src/opencl/ -- the library these 8 suites link -- is not added by any "
        "CMakeLists either, so wiring the tests in alone would fail configure "
        "wherever find_package(OpenCL REQUIRED) succeeds. 4,529 lines of host "
        "code and its tests, built nowhere; recorded in "
        "docs/internal/UNFIXED_AUDIT_FINDINGS.md rather than wired in blind, "
        "because no configure of this tree can be run without LLVM",
    "tests/utils/cuda_stub":
        "host stubs standing in for the CUDA runtime in "
        "scripts/standalone_check.sh; the CMake build uses the real one",
}

UNBUILT = {
    "tests/fileformat/ar_archive_format_probe_tests.cpp":
        "upstream fileformat suites; not carried through the LLVM 23.1.0 "
        "migration and not yet re-enabled -- see docs/internal/UNFIXED_AUDIT_FINDINGS.md",
    "tests/fileformat/coff_format_tests.cpp": "as above",
    "tests/fileformat/elf_format_tests.cpp": "as above",
    "tests/fileformat/format_detection_tests.cpp": "as above",
    "tests/fileformat/format_factory_tests.cpp": "as above",
    "tests/fileformat/intel_hex_format_20bit_tests.cpp": "as above",
    "tests/fileformat/intel_hex_format_tests.cpp": "as above",
    "tests/fileformat/intel_hex_token_test.cpp": "as above",
    "tests/fileformat/macho_format_tests.cpp": "as above",
    "tests/fileformat/pe_format_tests.cpp": "as above",
    "tests/fileformat/raw_data_format_tests.cpp": "as above",
    "tests/common/calling_convention_tests.cpp":
        "not listed in tests/common/CMakeLists.txt; same migration gap",
    "tests/managed_integration/fuzz/fuzz_pelib.cpp":
        "driven by scripts/standalone_fuzz.sh only -- PeLib needs no LLVM, so "
        "the harness does not need the RETDEC_FUZZ toolchain either",
}

# One balanced call. CMake source lists do not nest parentheses, but generator
# expressions do use them, so the token filter below drops anything with a '$'
# rather than trying to parse them.
CALL = re.compile(
    r"^[ \t]*(" + "|".join(COMMANDS) + r")[ \t]*\((.*?)\)",
    re.S | re.M | re.I,
)

def line_of(text, index):
    return text.count("\n", 0, index) + 1

missing = []
unfetched = []
named = set()
mentioned = set()
cu_listfiles = {}
checked = 0
listfiles = 0

for root in ROOTS:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [
            d for d in dirnames
            if d not in (".git", "build", "node_modules", "__pycache__")
        ]
        if "CMakeLists.txt" not in filenames:
            continue
        path = os.path.join(dirpath, "CMakeLists.txt")
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        listfiles += 1
        # A source can also be named through a wrapper function -- the fuzz
        # harnesses go through add_fuzz_target(name src), whose body is
        # `add_executable(${name} ${src})` and so resolves to nothing here. Any
        # source-looking token anywhere in this file counts as mentioned, which
        # is weaker than resolving the call but is the difference between
        # "nobody builds this" and "built by a macro".
        for m in re.findall(r"[A-Za-z0-9_./${}-]+\.(?:cpp|cc|cxx)", text):
            if "$" in m:
                continue
            cand = os.path.normpath(os.path.join(dirpath, m))
            if os.path.exists(cand):
                mentioned.add(os.path.relpath(cand, ".").replace(os.sep, "/"))
        # .cu entries are collected from the whole file rather than from the
        # target calls above: a source list normally reaches add_library through
        # a set() variable, and by the time it is an argument it is `${VAR}`.
        for m in re.finditer(r"[A-Za-z0-9_./-]+\.cu\b", text):
            cand = os.path.normpath(os.path.join(dirpath, m.group(0)))
            if not os.path.exists(cand):
                continue
            key = os.path.relpath(path, ".").replace(os.sep, "/")
            cu_listfiles.setdefault(key, []).append(
                (line_of(text, m.start()), m.group(0)))
        for call in CALL.finditer(text):
            body = call.group(2)
            # Blank out comments rather than deleting them: the reported
            # line number is derived from a token's offset inside this string,
            # so it has to keep the same length as the file it came from. A
            # '#' inside a quoted string would be misread, but no source list
            # in this tree has one.
            body = re.sub(r"#[^\n]*", lambda m: " " * len(m.group(0)), body)
            for tok in re.finditer(r"\S+", body):
                token = tok.group(0).strip('"')
                if not token or token in KEYWORDS:
                    continue
                # Variables, generator expressions and absolute paths that
                # depend on configure-time values are out of scope here.
                if "$" in token or token.startswith("-"):
                    continue
                if not token.endswith(SOURCE_SUFFIXES):
                    continue
                checked += 1
                candidate = os.path.normpath(os.path.join(dirpath, token))
                if os.path.exists(candidate):
                    # Remember it: the second pass asks the opposite question,
                    # and a source is "built" when SOME CMakeLists names it, not
                    # when the one in its own directory does. tests/llvmir2hll
                    # and tests/bin2llvmir list their subdirectories' files from
                    # the parent, which a per-directory answer gets wrong.
                    named.add(os.path.relpath(candidate, ".").replace(os.sep, "/"))
                    continue
                rel = os.path.relpath(candidate, ".").replace(os.sep, "/")
                if rel in FETCHABLE:
                    unfetched.append(rel)
                else:
                    # call.start(2) is where the argument list begins in
                    # the file, and the comment strip above preserves length,
                    # so the token's offset inside body is its offset in the
                    # file too.
                    missing.append(
                        (path, line_of(text, call.start(2) + tok.start()), token)
                    )

# ── second pass: a test source nobody builds ────────────────────────────────
#
# Three ways a test can fail to be built, and this used to see only the first:
#
#   1. a CMakeLists.txt covers the directory but does not name the file;
#   2. the directory has NO CMakeLists.txt at all -- `continue`d out of the walk
#      below, which is how tests/pdbparser/pdbparser_test.cpp came to be run by
#      scripts/standalone_check.sh and by nothing else;
#   3. the directory has a complete CMakeLists.txt that tests/CMakeLists.txt
#      never add_subdirectory()s -- which is how tests/opencl/, eight suites
#      with a working build file, came to be built nowhere at all.
#
# A check written to catch "nobody builds this" that skips the directories
# nobody builds is worse than none, because its OK is trusted.
#
# The question is asked against `named`, every source any CMakeLists in the tree
# resolves to, rather than against the one in the file's own directory:
# tests/llvmir2hll and tests/bin2llvmir list their subdirectories' sources from
# the parent, and a per-directory answer calls all 180 of those unbuilt.
unbuilt = []
unregistered = []
if ROOTS == ["."] or any(r.startswith("tests") for r in ROOTS):
    with open("tests/CMakeLists.txt", encoding="utf-8", errors="replace") as fh:
        top = fh.read()
    added = set(re.findall(
        r"(?:cond_)?add_subdirectory[ \t]*\([ \t]*([A-Za-z0-9_.-]+)", top))

    for dirpath, dirnames, filenames in os.walk("tests"):
        dirnames[:] = [
            d for d in dirnames
            if d not in (".git", "build", "node_modules", "__pycache__")
        ]
        rel_dir = dirpath.replace(os.sep, "/")
        if any(rel_dir == d or rel_dir.startswith(d + "/") for d in UNBUILT_DIRS):
            continue

        sources = sorted(n for n in filenames if n.endswith((".cpp", ".cc", ".cxx")))

        # A build file nothing includes builds nothing. Only immediate children
        # of tests/ are reached by tests/CMakeLists.txt.
        parts = rel_dir.split("/")
        if ("CMakeLists.txt" in filenames and len(parts) == 2
                and parts[1] not in added):
            unregistered.append(rel_dir)

        for name in sources:
            rel = f"{rel_dir}/{name}"
            if rel in named or rel in mentioned or rel in UNBUILT:
                continue
            unbuilt.append(rel)

# ── third pass: a .cu source CMake will silently drop ───────────────────────
#
# CMake compiles a .cu only when the CUDA language is enabled. A .cu named by a
# target in a project that has not enabled it belongs to no enabled language,
# and CMake does not warn, error, or configure-fail -- it drops the file from
# the target. Measured on a two-source toy project (a.cpp + b.cu, project(...
# CXX)): the archive built clean and contained a.cpp.o alone.
#
# That is how src/cuda_accel/ came to ship three of its eight objects in the
# default build, under a comment saying the .cu files "are compiled as plain
# C++" -- which nothing made true. It is invisible to every other check here,
# because the sources exist and are named; they are simply not built.
#
# The rule: a CMakeLists that names a .cu must either route it to the C++
# compiler (LANGUAGE CXX, which needs -x c++ alongside it -- GCC and Clang
# dispatch on the extension too and hand a bare .cu to the linker), or name only
# .cu files that are unreachable unless CUDA is enabled, which cannot be read
# off the text and is recorded in CU_CUDA_ONLY instead.
CU_CUDA_ONLY = {
    # gpu_scanner.cu is named only inside `if(RETDEC_CUDA_FOUND)`, and the
    # else-branch builds gpu_scanner_cpu.cpp in its place. Nothing is dropped.
    "src/utils/CMakeLists.txt",
}

cu_unbuilt = []
for cu_path, entries in sorted(cu_listfiles.items()):
    if cu_path in CU_CUDA_ONLY:
        continue
    with open(cu_path, encoding="utf-8", errors="replace") as fh:
        cu_text = fh.read()
    if "LANGUAGE CXX" in cu_text:
        continue
    cu_unbuilt.extend((cu_path, line, token) for line, token in entries)

for path, line, token in cu_unbuilt:
    print(f"{path}:{line}: names {token}, and this CMakeLists neither sets "
          f"LANGUAGE CXX for it nor is listed in CU_CUDA_ONLY")
    print("       CMake drops a .cu from the target, silently, when the CUDA "
          "language is not enabled")

for rel in unbuilt:
    print(f"{rel}: a test source no CMakeLists.txt names, so ctest never runs it")
    print("       add it to its CMakeLists, or give it a reason in UNBUILT in "
          "scripts/check_cmake_sources.sh")

for rel in unregistered:
    print(f"{rel}/CMakeLists.txt: tests/CMakeLists.txt never add_subdirectory()s "
          "this, so nothing it builds is built")
    print("       add it there, or give its sources a reason in UNBUILT in "
          "scripts/check_cmake_sources.sh")

for path, line, token in missing:
    print(f"{path}:{line}: names a source that does not exist: {token}")

for rel in sorted(set(unfetched)):
    print(f"note: {rel} is not in this tree yet; "
          f"scripts/fetch-large-files.sh downloads it")

print(f"checked {checked} source entries across {listfiles} CMakeLists.txt file(s)")
if UNBUILT:
    print(f"note: {len(UNBUILT)} test source(s) deliberately not built; "
          f"reasons in UNBUILT in this script")
if missing or unbuilt or unregistered or cu_unbuilt:
    if cu_unbuilt:
        print(f"FAIL: {len(cu_unbuilt)} .cu source(s) CMake would drop silently")
    if missing:
        print(f"FAIL: {len(missing)} missing source file(s)")
    if unbuilt:
        print(f"FAIL: {len(unbuilt)} test source(s) that nothing builds")
    if unregistered:
        print(f"FAIL: {len(unregistered)} test director(y/ies) tests/CMakeLists.txt "
              f"never adds")
    sys.exit(1)
print("OK: every source named by a CMake target exists or is fetchable, every "
      "test source is built, every test directory is reachable from "
      "tests/CMakeLists.txt, and no .cu source is silently dropped")
PY
