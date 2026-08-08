#!/usr/bin/env bash
# ingest_fuzz_crash.sh — Add a fuzzer crash to tests/crash_corpus/ (Part 10.3).
# Usage: bash scripts/ingest_fuzz_crash.sh <crash.bin> <pe|elf|macho>
set -euo pipefail

CRASH="${1:?crash file}"
KIND="${2:?pe|elf|macho}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/tests/crash_corpus/${KIND}/$(basename "${CRASH}")"
mkdir -p "$(dirname "${DEST}")"
cp "${CRASH}" "${DEST}"
sha256sum "${DEST}" | tee "${DEST}.sha256"
echo "Ingested ${DEST}"
echo "Add a ctest referencing this file."
