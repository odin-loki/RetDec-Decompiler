#!/bin/bash
# check_recompile.sh — Decompile a binary with RetDec then attempt to
# recompile each output function with gcc -c.  Reports which functions
# compile clean and which have errors.
#
# Usage:
#   ./check_recompile.sh <binary> [retdec-decompiler path]
#
# Dependencies: retdec, gcc, split (coreutils)

set -euo pipefail

BINARY="${1:-}"
RETDEC="${2:-retdec-decompiler}"

if [[ -z "$BINARY" ]]; then
    echo "Usage: $0 <binary> [path-to-retdec-decompiler]" >&2
    exit 1
fi

if ! command -v "$RETDEC" &>/dev/null; then
    echo "Error: retdec-decompiler not found at '$RETDEC'" >&2
    exit 1
fi

WORKDIR=$(mktemp -d /tmp/retdec_recompile_XXXX)
trap 'rm -rf "$WORKDIR"' EXIT

BASENAME=$(basename "$BINARY")
OUT_C="$WORKDIR/${BASENAME}.c"

echo "[*] Decompiling $BINARY ..."
"$RETDEC" "$BINARY" -o "$OUT_C" 2>/dev/null || {
    echo "Error: decompilation failed" >&2
    exit 1
}

if [[ ! -f "$OUT_C" ]]; then
    echo "Error: no output file produced" >&2
    exit 1
fi

echo "[*] Splitting functions ..."
# Extract each top-level function into its own file using a simple
# brace-depth tracker.  This is intentionally simple — it handles the
# common case of one-level-deep functions.
python3 - "$OUT_C" "$WORKDIR" <<'PYEOF'
import sys, os, re

src  = open(sys.argv[1]).read()
outd = sys.argv[2]

# Collect all lines, track brace depth to find function boundaries.
lines  = src.splitlines(keepends=True)
depth  = 0
start  = None
fname  = None
idx    = 0
preamble_lines = []

func_re = re.compile(r'^[\w\s\*]+\b(\w+)\s*\(')

while idx < len(lines):
    line = lines[idx]
    stripped = line.strip()

    if depth == 0:
        m = func_re.match(line)
        if m and not stripped.startswith('//') and not stripped.startswith('#'):
            # Likely a function definition — look ahead for '{'
            if '{' in line or (idx + 1 < len(lines) and '{' in lines[idx + 1]):
                fname = m.group(1)
                start = idx

    for ch in line:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1

    if depth == 0 and start is not None and fname is not None:
        func_src = ''.join(preamble_lines) + ''.join(lines[start:idx + 1])
        out_path = os.path.join(outd, f'func_{fname}.c')
        with open(out_path, 'w') as f:
            f.write(func_src)
        fname = None
        start = None

    if depth == 0 and start is None:
        preamble_lines.append(line)

    idx += 1
PYEOF

PASS=0
FAIL=0
FAIL_LIST=()

echo "[*] Compiling individual functions ..."
for func_c in "$WORKDIR"/func_*.c; do
    func_name=$(basename "$func_c" .c)
    obj="$WORKDIR/${func_name}.o"
    if gcc -c -w -x c \
           -fno-strict-aliasing \
           -D__builtin_va_list='void*' \
           "$func_c" -o "$obj" 2>/dev/null; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        FAIL_LIST+=("$func_name")
    fi
done

TOTAL=$((PASS + FAIL))
echo ""
echo "=== Recompilability report for: $BINARY ==="
echo "  Functions tested : $TOTAL"
echo "  Compiled clean   : $PASS"
echo "  Failed           : $FAIL"
if [[ ${#FAIL_LIST[@]} -gt 0 ]]; then
    echo ""
    echo "  Failed functions:"
    for f in "${FAIL_LIST[@]}"; do
        echo "    - $f"
    done
fi
echo ""
if [[ $TOTAL -gt 0 ]]; then
    PCT=$(( PASS * 100 / TOTAL ))
    echo "  Score: ${PCT}% recompilable"
fi
