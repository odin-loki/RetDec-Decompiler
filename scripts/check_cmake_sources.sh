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

for path, line, token in missing:
    print(f"{path}:{line}: names a source that does not exist: {token}")

for rel in sorted(set(unfetched)):
    print(f"note: {rel} is not in this tree yet; "
          f"scripts/fetch-large-files.sh downloads it")

print(f"checked {checked} source entries across {listfiles} CMakeLists.txt file(s)")
if missing:
    print(f"FAIL: {len(missing)} missing source file(s)")
    sys.exit(1)
print("OK: every source named by a CMake target exists or is fetchable")
PY
