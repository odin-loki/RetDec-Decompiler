#!/usr/bin/env bash
# unpack_and_decompile.sh — unpack packed binaries, then decompile with RetDec.
#
# Usage:
#   ./scripts/unpack_and_decompile.sh INPUT [-o OUTPUT] [--keep-unpacked]
#
# Steps:
#   1. Optional fileinfo probe (informational; does not block)
#   2. retdec-unpacker (via retdec-unpacker.py wrapper when present)
#   3. retdec-decompiler on unpacked file (or original if nothing to unpack)
#
# Exit codes: same as retdec-decompiler (0 = success).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

INPUT=""
OUTPUT=""
KEEP_UNPACKED=0
INSTALL_BIN="${RETDEC_ROOT}/install/linux/bin"
BUILD_BIN="${RETDEC_ROOT}/build/linux/bin"

usage() {
    cat <<'EOF'
Usage: unpack_and_decompile.sh INPUT [-o OUTPUT] [--keep-unpacked]

  INPUT            Binary to unpack and decompile
  -o, --output     Decompiler output path (default: INPUT.c)
  --keep-unpacked  Do not delete the intermediate unpacked file
EOF
}

resolve_tool() {
    local name="$1"
    if [[ -x "${INSTALL_BIN}/${name}" ]]; then
        echo "${INSTALL_BIN}/${name}"
        return 0
    fi
    if [[ -x "${BUILD_BIN}/${name}" ]]; then
        echo "${BUILD_BIN}/${name}"
        return 0
    fi
    if command -v "${name}" >/dev/null 2>&1; then
        command -v "${name}"
        return 0
    fi
    return 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        -o|--output)
            OUTPUT="$2"
            shift 2
            ;;
        --keep-unpacked)
            KEEP_UNPACKED=1
            shift
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            if [[ -z "${INPUT}" ]]; then
                INPUT="$1"
            else
                echo "Unexpected argument: $1" >&2
                exit 2
            fi
            shift
            ;;
    esac
done

if [[ -z "${INPUT}" ]]; then
    usage >&2
    exit 2
fi

if [[ ! -f "${INPUT}" ]]; then
    echo "Error: input file not found: ${INPUT}" >&2
    exit 1
fi

INPUT="$(cd "$(dirname "${INPUT}")" && pwd)/$(basename "${INPUT}")"

if [[ -z "${OUTPUT}" ]]; then
    base="${INPUT%.*}"
    [[ "${base}" == "${INPUT}" ]] && base="${INPUT}"
    OUTPUT="${base}.c"
fi

UNPACKER_PY="${SCRIPT_DIR}/retdec-unpacker.py"
FILEINFO_PY="${SCRIPT_DIR}/retdec-fileinfo.py"
DECOMPILER="$(resolve_tool retdec-decompiler)" || {
    echo "Error: retdec-decompiler not found (build/install or add to PATH)" >&2
    exit 1
}

if [[ -x "${FILEINFO_PY}" || -f "${FILEINFO_PY}" ]]; then
    echo "[unpack_and_decompile] Probing input with fileinfo..."
    python3 "${FILEINFO_PY}" "${INPUT}" --silent 2>/dev/null || true
fi

WORK_INPUT="${INPUT}"
UNPACKED="${INPUT}-unpacked"
UNPACK_RAN=0

if [[ -f "${UNPACKER_PY}" ]]; then
    echo "[unpack_and_decompile] Running retdec-unpacker..."
    set +e
    python3 "${UNPACKER_PY}" "${INPUT}" -o "${UNPACKED}" --extended-exit-codes
    unpack_rc=$?
    set -e
    # 0 = unpacked; 1/3 = partial success with output; 2 = nothing to do
    if [[ ${unpack_rc} -eq 0 || ${unpack_rc} -eq 1 || ${unpack_rc} -eq 3 ]]; then
        if [[ -f "${UNPACKED}" ]]; then
            WORK_INPUT="${UNPACKED}"
            UNPACK_RAN=1
            echo "[unpack_and_decompile] Using unpacked file: ${WORK_INPUT}"
        fi
    else
        echo "[unpack_and_decompile] Unpacker: nothing to do or failed (rc=${unpack_rc}); decompiling original."
    fi
else
    UNPACKER_BIN="$(resolve_tool retdec-unpacker 2>/dev/null || true)"
    if [[ -n "${UNPACKER_BIN}" ]]; then
        echo "[unpack_and_decompile] Running ${UNPACKER_BIN}..."
        set +e
        "${UNPACKER_BIN}" "${INPUT}" -o "${UNPACKED}"
        unpack_rc=$?
        set -e
        if [[ ${unpack_rc} -eq 0 && -f "${UNPACKED}" ]]; then
            WORK_INPUT="${UNPACKED}"
            UNPACK_RAN=1
        fi
    fi
fi

echo "[unpack_and_decompile] Decompiling: ${WORK_INPUT} -> ${OUTPUT}"
"${DECOMPILER}" -o "${OUTPUT}" "${WORK_INPUT}"
dec_rc=$?

if [[ ${UNPACK_RAN} -eq 1 && ${KEEP_UNPACKED} -eq 0 ]]; then
    rm -f "${UNPACKED}"
fi

exit "${dec_rc}"
