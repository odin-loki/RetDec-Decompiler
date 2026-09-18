#!/usr/bin/env python3
"""PORT-01 -- no target links a bare Unix library name.

Why this exists
---------------
`target_link_libraries(retdec-jvm-parser PUBLIC retdec-bc-module z)` is two
mistakes in one token. `z` is the Unix spelling of a library file, so MSVC
cannot resolve it; and a bare name carries no include directory, so the
compiler never learns where zlib.h is. Native Windows died on the second of
those long before it could reach the first:

    src\\jvm_parser\\jvm_jar_reader.cpp(26): fatal error C1083:
        Cannot open include file: 'zlib.h': No such file or directory

Every `ctest-windows` run from 2026-08-25 to 2026-09-17 failed there, and
nothing that runs on Linux could see it: on Linux the header is in /usr/include
and `-lz` resolves, so the same line is correct by accident.

That is the point of this check. It runs on Linux, in seconds, and it fails on
a defect that only Windows can otherwise report -- and only after a build that
takes hours.

What counts
-----------
A bare name is flagged when it is a known Unix library spelling. Names with a
`::` (an imported target), a path separator, a generator expression, or a file
extension are targets or files and are left alone, as are the CMake keywords.

A link inside a block whose condition names UNIX, APPLE, LINUX, MINGW or
NOT WIN32 is deliberate and is skipped -- the platform has been stated.

Usage:
  python3 scripts/ci/check_portable_link_names.py
  python3 scripts/ci/check_portable_link_names.py --self-test
"""

import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Library names that mean a file on Unix and nothing on MSVC. Kept explicit:
# a list of every library in the world would flag project targets.
UNIX_LIBS = {
    "z", "dl", "rt", "m", "pthread", "crypto", "ssl", "bz2", "lzma",
    "curl", "sqlite3", "ncurses", "tinfo", "uuid", "iconv", "resolv",
    "util", "stdc++fs", "atomic",
}

KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE",
            "LINK_INTERFACE_LIBRARIES", "optimized", "debug", "general"}

# Conditions that state the platform, so a bare name under them is a choice
# rather than an oversight. MSVC and WIN32 are in the list for the same reason
# the Unix names are, and were added after this check flagged `uuid` inside an
# elseif(MSVC) that links LLVM's Windows system libraries: uuid.lib is the
# Windows SDK's and naming it there is correct. A rule that only recognised
# Unix platform statements was half a rule.
PLATFORM_COND = re.compile(
    r"\b(UNIX|APPLE|LINUX|MINGW|CYGWIN|ANDROID|BSD|MSVC|WIN32|MSYS)\b",
    re.IGNORECASE)


def scan(text, path):
    """Yield (line number, library) for every bare Unix library name linked."""
    findings = []
    cond_stack = []          # one entry per open if(), True when it names a platform
    i = 0
    lines = text.splitlines()
    while i < len(lines):
        line = lines[i]
        stripped = line.split("#", 1)[0].strip()

        m = re.match(r"^if\s*\((.*)", stripped, re.IGNORECASE)
        if m:
            cond_stack.append(bool(PLATFORM_COND.search(m.group(1))))
        elif re.match(r"^elseif\s*\((.*)", stripped, re.IGNORECASE):
            if cond_stack:
                cond_stack[-1] = bool(PLATFORM_COND.search(stripped))
        elif re.match(r"^endif\s*\(", stripped, re.IGNORECASE):
            if cond_stack:
                cond_stack.pop()

        if re.match(r"^target_link_libraries\s*\(", stripped, re.IGNORECASE):
            # Collect the call, which usually spans lines.
            depth = 0
            body = []
            while i < len(lines):
                cur = lines[i].split("#", 1)[0]
                depth += cur.count("(") - cur.count(")")
                body.append((i, cur))
                if depth <= 0:
                    break
                i += 1
            if not any(cond_stack):
                for ln, cur in body:
                    for tok in re.findall(r"[^\s()]+", cur):
                        if tok in KEYWORDS:
                            continue
                        if tok in UNIX_LIBS:
                            findings.append((ln + 1, tok))
        i += 1
    return findings


def check(paths):
    bad = []
    for p in paths:
        for ln, lib in scan(p.read_text(encoding="utf-8", errors="replace"), p):
            bad.append((p, ln, lib))
    return bad


def self_test():
    """The check has to fail on the line that broke Windows, and pass on its fix."""
    broke = """
add_library(x a.cpp)
target_link_libraries(x
    PUBLIC
        some-target
        z
)
"""
    fixed = """
add_library(x a.cpp)
target_link_libraries(x
    PUBLIC
        some-target
    PRIVATE
        retdec::deps::zlib
)
"""
    stated = """
add_library(x a.cpp)
if(UNIX AND NOT APPLE)
    target_link_libraries(x PRIVATE rt)
endif()
"""
    windows = """
add_library(x a.cpp)
if(MSVC)
    target_link_libraries(x INTERFACE ntdll uuid ws2_32)
endif()
"""
    with tempfile.TemporaryDirectory() as td:
        for name, text, want in (("broke", broke, 1), ("fixed", fixed, 0),
                                 ("stated", stated, 0),
                                 ("windows", windows, 0)):
            p = Path(td) / f"{name}.txt"
            p.write_text(text)
            got = len(scan(text, p))
            if got != want:
                print(f"PORT-01: FAIL self-test '{name}' expected {want} "
                      f"finding(s), got {got}", file=sys.stderr)
                return 1
    print("PORT-01: self-test OK -- a bare `z` is flagged, an imported target "
          "and a stated platform are not")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()

    paths = sorted(
        p for p in list(ROOT.glob("src/**/CMakeLists.txt"))
        + list(ROOT.glob("deps/**/CMakeLists.txt"))
        + list(ROOT.glob("tests/**/CMakeLists.txt"))
        + [ROOT / "CMakeLists.txt"]
        if p.is_file() and "build/" not in str(p.relative_to(ROOT))
    )
    if len(paths) < 50:
        print(f"PORT-01: FAIL only {len(paths)} CMakeLists.txt found; the glob "
              f"is not reaching the tree", file=sys.stderr)
        return 1

    bad = check(paths)
    if bad:
        for p, ln, lib in bad:
            print(f"{p.relative_to(ROOT)}:{ln}: links the bare Unix library "
                  f"name '{lib}'", file=sys.stderr)
        print(f"PORT-01: FAIL {len(bad)} bare Unix library name(s). MSVC cannot "
              f"resolve them and they carry no include directory. Link an "
              f"imported target instead, or state the platform with an "
              f"if(UNIX)/if(APPLE) around the call.", file=sys.stderr)
        return 1

    print(f"PORT-01: OK {len(paths)} CMakeLists.txt, no bare Unix library names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
