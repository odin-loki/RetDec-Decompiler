#!/usr/bin/env bash
# Compile tiny C corpus sources into native regression binaries.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/tests/decompiler/corpus/bin}"
SRC_DIR="${REPO_ROOT}/tests/decompiler/corpus/sources"
FIX_DIR="${REPO_ROOT}/tests/decompiler/corpus/fixtures"

mkdir -p "${OUT_DIR}" "${FIX_DIR}"

compile_c() {
    local src="$1"
    local dest="$2"
    if [[ -f "${dest}" ]]; then
        return 0
    fi
    if command -v gcc >/dev/null 2>&1; then
        gcc -O1 -o "${dest}" "${src}"
        return 0
    fi
    if command -v clang >/dev/null 2>&1; then
        clang -O1 -o "${dest}" "${src}"
        return 0
    fi
    return 1
}

built=0
for name in hello vector_sort; do
    src="${SRC_DIR}/${name}.c"
    if [[ ! -f "${src}" ]]; then
        echo "warning: missing source ${src}" >&2
        continue
    fi
    dest="${OUT_DIR}/${name}"
    if compile_c "${src}" "${dest}"; then
        echo "built ${dest}"
        built=$((built + 1))
    else
        echo "warning: failed to compile ${name}.c" >&2
    fi
done

fib_src="${REPO_ROOT}/tests/test_binaries/fib.c"
fib_dest="${OUT_DIR}/fib_smoke"
if [[ -f "${fib_src}" && ! -f "${fib_dest}" ]]; then
    if compile_c "${fib_src}" "${fib_dest}"; then
        echo "built ${fib_dest}"
        built=$((built + 1))
    fi
fi

wasm_dest="${FIX_DIR}/minimal.wasm"
if [[ ! -f "${wasm_dest}" ]]; then
    printf '\x00asm\x01\x00\x00\x00' > "${wasm_dest}"
    echo "wrote ${wasm_dest}"
fi

pyc_dest="${FIX_DIR}/hello.pyc"
py_src="${SRC_DIR}/hello.py"
if [[ -f "${py_src}" && ! -f "${pyc_dest}" ]]; then
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import py_compile; py_compile.compile('${py_src}', cfile='${pyc_dest}', doraise=True)"
        echo "built ${pyc_dest}"
    else
        echo "warning: python3 not found; skipped hello.pyc (see corpus/README.md)" >&2
    fi
fi

echo "corpus fixtures: ${built} native binary(ies) in ${OUT_DIR}"
