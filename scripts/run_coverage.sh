#!/usr/bin/env bash
# run_coverage.sh — Build RetDec with gcov instrumentation, run unit and
# integration tests, and produce an lcov HTML report in docs/coverage/.
#
# Usage (from repo root in WSL):
#   bash scripts/run_coverage.sh [--preset <name>] [--open]
#
# Options:
#   --preset NAME    CMake preset to use (default: core-coverage)
#   --open           Open the HTML report in the browser when done
#
# Prerequisites:
#   GCC or Clang with --coverage support, lcov, genhtml, cmake, ninja
#   Install: sudo apt install lcov

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="core-coverage"
OPEN_BROWSER=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --preset) PRESET="$2"; shift 2;;
        --open)   OPEN_BROWSER=1; shift;;
        *)        echo "Unknown option: $1" >&2; exit 1;;
    esac
done

# Root CMakePresets place the binary tree under build/linux or build/windows (not build/<preset>).
case "$(uname -s 2>/dev/null)" in
  MINGW*|MSYS*|CYGWIN*) BUILD_DIR="$REPO_ROOT/build/windows" ;;
  *)                     BUILD_DIR="$REPO_ROOT/build/linux" ;;
esac
REPORT_DIR="$REPO_ROOT/docs/coverage"

echo "=== RetDec Coverage Build ==="
echo "Preset:      $PRESET"
echo "Build dir:   $BUILD_DIR"
echo "Report dir:  $REPORT_DIR"
echo ""

# Ensure lcov is available
if ! command -v lcov &>/dev/null; then
    echo "ERROR: lcov not found. Install with: sudo apt install lcov" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 1. Configure
# ---------------------------------------------------------------------------
echo "[1/5] Configuring with preset '$PRESET'..."
cmake --preset "$PRESET" -S "$REPO_ROOT" 2>&1 | tail -5

# ---------------------------------------------------------------------------
# 2. Build
# ---------------------------------------------------------------------------
echo "[2/5] Building..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# 3. Baseline coverage (zero-counters capture)
# ---------------------------------------------------------------------------
echo "[3/5] Capturing baseline coverage..."
lcov \
    --capture --initial \
    --directory "$BUILD_DIR" \
    --output-file "$BUILD_DIR/coverage_baseline.info" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors mismatch \
    --quiet

# ---------------------------------------------------------------------------
# 4. Run tests
# ---------------------------------------------------------------------------
echo "[4/5] Running tests..."
(
    cd "$BUILD_DIR"
    ctest --output-on-failure --parallel "$(nproc)" \
          --label-regex "managed|unit" \
          --timeout 300 \
    || echo "[warn] Some tests failed; continuing with coverage collection."
)

# Run managed integration tests separately if retdec-decompiler is built
RETDEC_BIN="$BUILD_DIR/src/retdec-decompiler/retdec-decompiler"
if [[ -x "$RETDEC_BIN" ]]; then
    echo "  Running managed integration tests..."
    python3 "$REPO_ROOT/tests/managed_integration/run_integration_tests.py" \
        --retdec-bin "$RETDEC_BIN" \
        --fixtures "$REPO_ROOT/tests/managed_integration/fixtures" \
        --golden   "$REPO_ROOT/tests/managed_integration/golden" \
        --timeout 120 \
        --jobs "$(nproc)" \
        --junit "$BUILD_DIR/managed_integration.xml" \
    || echo "[warn] Integration tests had failures."
fi

# ---------------------------------------------------------------------------
# 5. Capture and merge coverage, generate HTML report
# ---------------------------------------------------------------------------
echo "[5/5] Generating coverage report..."

lcov \
    --capture \
    --directory "$BUILD_DIR" \
    --output-file "$BUILD_DIR/coverage_test.info" \
    --rc lcov_branch_coverage=1 \
    --ignore-errors mismatch \
    --quiet

lcov \
    --add-tracefile "$BUILD_DIR/coverage_baseline.info" \
    --add-tracefile "$BUILD_DIR/coverage_test.info" \
    --output-file   "$BUILD_DIR/coverage_total.info" \
    --rc lcov_branch_coverage=1 \
    --quiet

# Remove third-party and system paths from the report
lcov \
    --remove "$BUILD_DIR/coverage_total.info" \
        '/usr/*' \
        '*/deps/*' \
        '*/tests/*' \
        '*/build/*' \
    --output-file "$BUILD_DIR/coverage_filtered.info" \
    --rc lcov_branch_coverage=1 \
    --quiet

mkdir -p "$REPORT_DIR"
genhtml \
    "$BUILD_DIR/coverage_filtered.info" \
    --output-directory "$REPORT_DIR" \
    --branch-coverage \
    --title "RetDec Coverage ($(date +%Y-%m-%d))" \
    --quiet

# Summary
lcov \
    --summary "$BUILD_DIR/coverage_filtered.info" \
    --rc lcov_branch_coverage=1 \
    2>&1 | grep -E "lines|branches|functions"

echo ""
echo "HTML report: $REPORT_DIR/index.html"

if [[ $OPEN_BROWSER -eq 1 ]]; then
    xdg-open "$REPORT_DIR/index.html" 2>/dev/null || \
    open "$REPORT_DIR/index.html" 2>/dev/null || \
    echo "(Could not open browser automatically)"
fi
