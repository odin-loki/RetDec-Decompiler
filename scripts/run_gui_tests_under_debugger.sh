#!/usr/bin/env bash
# run_gui_tests_under_debugger.sh
# ---------------------------------------------------------------------------
# Run retdec-gui-tests under gdb so any crash gives a stack trace and a
# core file. Works for both manual debugging and CI.
#
# Usage:
#   ./scripts/run_gui_tests_under_debugger.sh
#   ./scripts/run_gui_tests_under_debugger.sh --filter "ProgressPanelTest.*"
#   ./scripts/run_gui_tests_under_debugger.sh --no-debugger
# ---------------------------------------------------------------------------
set -euo pipefail

BUILD_DIR=""
FILTER=""
TIMEOUT_SEC=600
NO_DEBUGGER=0
OUT_DIR="gui-tests-debug-artifacts"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)   BUILD_DIR="$2"; shift 2 ;;
    --filter)      FILTER="$2"; shift 2 ;;
    --timeout)     TIMEOUT_SEC="$2"; shift 2 ;;
    --no-debugger) NO_DEBUGGER=1; shift ;;
    --out)         OUT_DIR="$2"; shift 2 ;;
    -h|--help)
      grep -E '^#' "$0" | sed -E 's/^#\s?//'; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

step() { printf '[gui-test-dbg] %s\n' "$*"; }

if [[ -z "$BUILD_DIR" ]]; then
  for cand in build/linux build/Release build/Debug build; do
    if [[ -x "$cand/tests/gui/retdec-gui-tests" ]]; then
      BUILD_DIR="$cand"; break
    fi
  done
fi

if [[ -z "$BUILD_DIR" ]]; then
  echo "Could not auto-detect build dir; pass --build-dir." >&2
  exit 2
fi

TEST_EXE="$(readlink -f "$BUILD_DIR/tests/gui/retdec-gui-tests")"
step "Tests: $TEST_EXE"

mkdir -p "$OUT_DIR"
LOG_FILE="$OUT_DIR/tests.log"
CORE_FILE="$OUT_DIR/tests.core"
rm -f "$LOG_FILE" "$CORE_FILE"

export RETDEC_GUI_HEADLESS=1
export QT_QPA_PLATFORM=offscreen

GTEST_ARGS=()
if [[ -n "$FILTER" ]]; then GTEST_ARGS+=("--gtest_filter=$FILTER"); fi
GTEST_ARGS+=("--gtest_color=no")

if [[ "$NO_DEBUGGER" -eq 1 ]]; then
  step "Running without debugger."
  if command -v timeout >/dev/null 2>&1; then
    timeout --foreground "$TIMEOUT_SEC" "$TEST_EXE" "${GTEST_ARGS[@]}" \
      > "$LOG_FILE" 2>&1
  else
    "$TEST_EXE" "${GTEST_ARGS[@]}" > "$LOG_FILE" 2>&1
  fi
  exit $?
fi

GDB="$(command -v gdb || true)"
if [[ -z "$GDB" ]]; then
  step "gdb not found; falling back to no-debugger mode."
  "$0" --build-dir "$BUILD_DIR" --filter "$FILTER" \
       --timeout "$TIMEOUT_SEC" --no-debugger --out "$OUT_DIR"
  exit $?
fi
step "gdb: $GDB"

GDB_SCRIPT="$OUT_DIR/gdb.script"
cat > "$GDB_SCRIPT" <<EOF
set pagination off
set logging file $LOG_FILE
set logging overwrite on
set logging redirect off
set logging enabled on
set confirm off
handle SIGSEGV stop print nopass
handle SIGABRT stop print nopass
run
info threads
thread apply all bt full
generate-core-file $CORE_FILE
quit
EOF

step "Launching: $GDB -batch -x $GDB_SCRIPT --args $TEST_EXE ${GTEST_ARGS[*]}"
ulimit -c unlimited || true

if command -v timeout >/dev/null 2>&1; then
  set +e
  timeout --foreground "$TIMEOUT_SEC" \
      "$GDB" -batch -x "$GDB_SCRIPT" --args "$TEST_EXE" "${GTEST_ARGS[@]}"
  RET=$?
  set -e
  if [[ "$RET" -eq 124 ]]; then
    step "Timeout after ${TIMEOUT_SEC}s."
    exit 124
  fi
else
  set +e
  "$GDB" -batch -x "$GDB_SCRIPT" --args "$TEST_EXE" "${GTEST_ARGS[@]}"
  RET=$?
  set -e
fi
step "gdb exited with $RET. Log: $LOG_FILE"
[[ -f "$CORE_FILE" ]] && step "Core: $CORE_FILE"
exit "$RET"
