#!/usr/bin/env bash
# differential_gate_triton.sh — Triton/D-Helix differential gate (step 20).
# Usage: bash scripts/differential_gate_triton.sh <original.c> <refined.c> [--mode auto|stdout|fuzz|triton]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ORIG="${1:?original.c}"
REF="${2:?refined.c}"
shift 2 || true

exec python3 "${ROOT}/scripts/triton_diff_gate.py" "${ORIG}" "${REF}" "$@"
