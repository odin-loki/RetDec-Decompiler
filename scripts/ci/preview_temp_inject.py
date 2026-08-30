#!/usr/bin/env python3
"""Preview undeclared-temp injection on existing fork artifacts (same rules as C++)."""
import re
import subprocess
from pathlib import Path

ART = Path(__file__).resolve().parents[2] / "tests/decompilebench/artifacts/fork"
IDENT = re.compile(r"\b(result|v(?:[1-9]|1[0-6]))\b")
DECL = re.compile(
    r"\b(?:int64_t|uint64_t|int32_t|uint32_t|int16_t|uint16_t|int8_t|uint8_t|int|long|unsigned|char|bool|size_t)\s+(result|v(?:[1-9]|1[0-6]))\b"
)
FN = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{",
    re.M,
)
CONTROL = {"if", "for", "while", "switch", "else"}


def matching_brace(s: str, open_i: int) -> int:
    depth = 0
    i = open_i
    while i < len(s):
        if s[i] == "{":
            depth += 1
        elif s[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def inject(src: str) -> str:
    out = []
    i = 0
    for m in FN.finditer(src):
        if m.group(1) in CONTROL:
            continue
        out.append(src[i : m.end()])
        close = matching_brace(src, m.end() - 1)
        if close < 0:
            i = m.end()
            continue
        body = src[m.end() : close]
        declared = set(DECL.findall(body))
        used = set(IDENT.findall(body))
        missing = sorted(used - declared, key=lambda n: (n != "result", n))
        if missing:
            decl = "".join(f"    int64_t {n} = 0;\n" for n in missing)
            out.append("\n" + decl)
        out.append(body)
        out.append("}")
        i = close + 1
    out.append(src[i:])
    return "".join(out)


def main() -> None:
    raw = sorted(
        p
        for p in ART.glob("*.c")
        if not p.name.endswith(".buildable.c") and not p.name.endswith(".refined.c")
        and "-gcc-O0-" in p.name
    )
    ok = 0
    for path in raw:
        text = path.read_text(encoding="utf-8", errors="replace")
        repaired = '#include <stdint.h>\n#include <stddef.h>\n' + inject(text)
        tmp = path.with_suffix(".preview.c")
        tmp.write_text(repaired, encoding="utf-8")
        proc = subprocess.run(
            ["gcc", "-fsyntax-only", "-std=gnu11", "-w", str(tmp)],
            capture_output=True,
            text=True,
        )
        status = "PASS" if proc.returncode == 0 else "FAIL"
        if proc.returncode == 0:
            ok += 1
        else:
            err = (proc.stderr or "").splitlines()[:3]
            print(f"{status} {path.name}: {err}")
        print(f"{status} {path.name}")
    print(f"tu_valid preview {ok}/{len(raw)}")


if __name__ == "__main__":
    main()
