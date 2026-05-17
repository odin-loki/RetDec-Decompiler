#!/bin/bash
# =============================================================================
# run_full_pipeline.sh — one-shot: compile → decompile → analyze coverage
#
# Run from WSL:
#   bash tests/test_binaries/run_full_pipeline.sh
#
# Produces:
#   /tmp/rtest_out/full_cov_O1.c   — primary decompiled output
#   /tmp/rtest_out/coverage.txt    — optimizer coverage report
# =============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRCDIR="$SCRIPT_DIR"
OUT=/tmp/rtest_out

echo "===== Step 1: Compile & Decompile ====="
bash "$SRCDIR/run_decompile.sh" 2>&1 | tee "$OUT/decompile.log"

echo ""
echo "===== Step 2: Analyze Coverage ====="
bash "$SRCDIR/analyze_coverage.sh" 2>&1 | tee "$OUT/coverage.txt"

echo ""
echo "Coverage report saved to: $OUT/coverage.txt"
echo "Primary decompiled output: $OUT/full_cov_O1.c"
