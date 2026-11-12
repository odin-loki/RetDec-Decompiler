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

# lcov 2.x turns several conditions lcov 1.x tolerated into hard errors:
# "inconsistent" fires on gtest's TEST() bodies (the macro moves a function's
# end line), "unused" on a --remove pattern that matched nothing, and the
# rest on GCC 15's exception-branch tags. None of them says the data is
# wrong, and the scheduled coverage job died on the first. lcov 1.x rejects
# the 2.x-only categories, so pick the list by version.
LCOV_MAJOR="$(lcov --version 2>&1 | grep -oE '[0-9]+\.[0-9]+' | head -n1 | cut -d. -f1)"
if [[ "${LCOV_MAJOR:-1}" -ge 2 ]]; then
    LCOV_IGNORE=(--ignore-errors mismatch,mismatch,inconsistent,inconsistent,unused,unused,empty,negative,source,gcov,version,version)
    GENHTML_IGNORE=(--ignore-errors inconsistent,inconsistent,source,unmapped,empty)
else
    # 1.x knows only gcov, source and graph; "mismatch" is a 2.x category and
    # 1.x geninfo rejects it ("unknown argument for --ignore-errors").
    LCOV_IGNORE=(--ignore-errors source)
    GENHTML_IGNORE=()
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

# Capture RetDec's own object directories, not the whole build tree.
# external/ is EP_PREFIX: the ExternalProject source and build trees (LLVM and
# friends). LLVM's sources ship test inputs such as
# llvm/test/tools/llvm-cov/gcov/Inputs/*.gcda with no matching .gcno, and
# lcov 2.x stops with "ERROR: (path) cannot process .gcda file" on the first
# one. deps/ holds the vendored dependencies and their ExternalProjects (yara);
# everything it compiles is removed from the report below, so skip it too.
# Every other top-level directory (src/, tests/, third_party/ for the
# first-party Forge/FuseUI, ...) is captured as before.
LCOV_DIRS=()
for dir in "$BUILD_DIR"/*/; do
    dir="${dir%/}"
    case "${dir##*/}" in
        external|deps) continue ;;
    esac
    LCOV_DIRS+=(--directory "$dir")
done
if [[ ${#LCOV_DIRS[@]} -eq 0 ]]; then
    echo "ERROR: no object directories to capture under $BUILD_DIR" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# 3. Baseline coverage (zero-counters capture)
# ---------------------------------------------------------------------------
echo "[3/5] Capturing baseline coverage..."
lcov \
    --capture --initial \
    "${LCOV_DIRS[@]}" \
    --output-file "$BUILD_DIR/coverage_baseline.info" \
    --rc lcov_branch_coverage=1 \
    "${LCOV_IGNORE[@]}" \
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
    "${LCOV_DIRS[@]}" \
    --output-file "$BUILD_DIR/coverage_test.info" \
    --rc lcov_branch_coverage=1 \
    "${LCOV_IGNORE[@]}" \
    --quiet

lcov \
    --add-tracefile "$BUILD_DIR/coverage_baseline.info" \
    --add-tracefile "$BUILD_DIR/coverage_test.info" \
    --output-file   "$BUILD_DIR/coverage_total.info" \
    --rc lcov_branch_coverage=1 \
    "${LCOV_IGNORE[@]}" \
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
    "${LCOV_IGNORE[@]}" \
    --quiet

mkdir -p "$REPORT_DIR"
genhtml \
    "$BUILD_DIR/coverage_filtered.info" \
    --output-directory "$REPORT_DIR" \
    --branch-coverage \
    --title "RetDec Coverage ($(date +%Y-%m-%d))" \
    "${GENHTML_IGNORE[@]}" \
    --quiet

# Summary
lcov \
    --summary "$BUILD_DIR/coverage_filtered.info" \
    --rc lcov_branch_coverage=1 \
    "${LCOV_IGNORE[@]}" \
    2>&1 | grep -E "lines|branches|functions"

echo ""
echo "HTML report: $REPORT_DIR/index.html"

if [[ $OPEN_BROWSER -eq 1 ]]; then
    xdg-open "$REPORT_DIR/index.html" 2>/dev/null || \
    open "$REPORT_DIR/index.html" 2>/dev/null || \
    echo "(Could not open browser automatically)"
fi
