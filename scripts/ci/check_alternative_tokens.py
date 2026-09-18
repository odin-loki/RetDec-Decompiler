#!/usr/bin/env python3
"""PORT-02 -- no C++ source uses the alternative operator spellings.

Why this exists
---------------
`or`, `and`, `not` and the rest are standard C++ and mean exactly `||`, `&&`
and `!`. gcc and clang accept them without comment. MSVC does not, unless it is
given /permissive- or the translation unit includes <iso646.h>, and what it
says instead is a cascade that names neither the token nor the cause:

    x86.cpp(6480): error C2065: 'or': undeclared identifier
    x86.cpp(6480): error C2146: syntax error: missing ';' before identifier 'i'
    x86.cpp(6489): error C2653: 'Capstone2LlvmIrTranslatorX86_impl':
        is not a class or namespace name

One `or` in one line of src/capstone2llvmir/x86/x86.cpp did that, and it had
been sitting there through every Linux build because on Linux it is correct.
Windows never reached it -- the build died earlier, on zlib -- so it cost a
35-minute round to find.

Raw strings
-----------
Most apparent hits in this tree are not code: the bin2llvmir and llvmir2hll
tests hold LLVM IR in raw string literals, and LLVM IR spells its instructions
`and`, `or` and `xor`. A checker that did not understand R"(...)" would report
seventy of those and one real defect. Comments, ordinary strings, character
literals and raw strings are all blanked before the search, and the blanking
preserves line numbers so the ones it does report can be found.

Usage:
  python3 scripts/ci/check_alternative_tokens.py
  python3 scripts/ci/check_alternative_tokens.py --self-test
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# C++ [lex.digraph]. Every one of these is a reserved alternative spelling, so
# an occurrence outside a comment or a literal is always the operator.
TOKENS = ("or", "and", "not", "xor", "bitor", "bitand", "compl",
          "or_eq", "and_eq", "xor_eq", "not_eq")
TOKEN_RE = re.compile(r"(?<![\w:.])(" + "|".join(TOKENS) + r")(?![\w])")

REPLACEMENT = {
    "or": "||", "and": "&&", "not": "!", "xor": "^", "bitor": "|",
    "bitand": "&", "compl": "~", "or_eq": "|=", "and_eq": "&=",
    "xor_eq": "^=", "not_eq": "!=",
}

SUFFIXES = (".cpp", ".cc", ".cxx", ".h", ".hpp")


def blank(match):
    """Replace a match with spaces, keeping every newline where it was."""
    return re.sub(r"[^\n]", " ", match.group(0))


def strip_literals(text):
    text = re.sub(r'R"([^(\s\\]*)\(.*?\)\1"', blank, text, flags=re.S)  # raw
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)                # block
    text = re.sub(r"//[^\n]*", blank, text)                             # line
    text = re.sub(r'"(\\.|[^"\\\n])*"', blank, text)                    # string
    text = re.sub(r"'(\\.|[^'\\\n])*'", blank, text)                    # char
    return text


def scan(text):
    """[(line number, token)] for each alternative operator in real code."""
    found = []
    for n, line in enumerate(strip_literals(text).splitlines(), 1):
        for m in TOKEN_RE.finditer(line):
            found.append((n, m.group(1)))
    return found


def self_test():
    code = 'if (a or b) { }\n'
    raw = 'const char* ir = R"(\n  %r = and i32 %x, 1\n  %s = or i32 %r, 2\n)";\n'
    comment = '// this or that, and the other\n/* not here either */\n'
    string = 'puts("a or b and c");\n'
    late = 'int x;\nint y;\nif (p and q) { }\n'
    for name, text, want in (("code", code, [(1, "or")]),
                             ("raw string", raw, []),
                             ("comment", comment, []),
                             ("string", string, []),
                             ("line number", late, [(3, "and")])):
        got = scan(text)
        if got != want:
            print(f"PORT-02: FAIL self-test '{name}' expected {want}, got {got}",
                  file=sys.stderr)
            return 1
    print("PORT-02: self-test OK -- `or` in code is flagged; LLVM IR in a raw "
          "string, comments and strings are not, and line numbers survive")
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return self_test()

    paths = sorted(
        p for d in ("src", "include", "tests")
        for p in (ROOT / d).rglob("*")
        if p.suffix in SUFFIXES and p.is_file()
    )
    if len(paths) < 500:
        print(f"PORT-02: FAIL only {len(paths)} source file(s) found; the glob "
              f"is not reaching the tree", file=sys.stderr)
        return 1

    bad = []
    for p in paths:
        for ln, tok in scan(p.read_text(encoding="utf-8", errors="replace")):
            bad.append((p, ln, tok))

    if bad:
        for p, ln, tok in bad:
            print(f"{p.relative_to(ROOT)}:{ln}: '{tok}' is the alternative "
                  f"spelling of '{REPLACEMENT[tok]}'; MSVC rejects it",
                  file=sys.stderr)
        print(f"PORT-02: FAIL {len(bad)} alternative operator token(s). They "
              f"are standard C++ and gcc and clang accept them, but MSVC does "
              f"not without /permissive- and reports a syntax cascade that "
              f"names neither the token nor the cause.", file=sys.stderr)
        return 1

    print(f"PORT-02: OK {len(paths)} source file(s), no alternative operator "
          f"tokens")
    return 0


if __name__ == "__main__":
    sys.exit(main())
