#!/usr/bin/env bash
# flamegraph_profile.sh — Linux perf + flamegraph scaffold (Part 11.1).
# Usage: bash scripts/flamegraph_profile.sh <binary> [output.svg]
set -euo pipefail

BIN="${1:?binary path}"
OUT="${2:-flamegraph.svg}"
FREQ="${RETDEC_PERF_FREQ:-99}"

if ! command -v perf >/dev/null 2>&1; then
	echo "perf not found — install linux-tools-common / perf" >&2
	exit 1
fi

DATA="$(mktemp /tmp/retdec-perf-XXXX.data)"
trap 'rm -f "${DATA}"' EXIT

echo "==> Recording ${FREQ}Hz for 30s: ${BIN}"
perf record -F "${FREQ}" -g -o "${DATA}" -- "${BIN}" --help >/dev/null 2>&1 || true

if command -v stackcollapse-perf.pl >/dev/null 2>&1 && command -v flamegraph.pl >/dev/null 2>&1; then
	perf script -i "${DATA}" | stackcollapse-perf.pl | flamegraph.pl > "${OUT}"
	echo "Wrote ${OUT}"
else
	perf report -i "${DATA}" --stdio | head -n 80
	echo "Install FlameGraph (stackcollapse-perf.pl, flamegraph.pl) for SVG output" >&2
	exit 2
fi
