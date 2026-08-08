#!/usr/bin/env bash
# doctor.sh - Check common RetDec build prerequisites (read-only).
#
# Usage (works when not executable):
#   bash scripts/doctor.sh
#   ./scripts/doctor.sh
#
# Checks: CMake >= 3.26, fetch-large-files marker, Qt6 hint, git-lfs,
#         python3, perl. On Windows Git Bash also reports NSIS/makensis.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=26
LARGE_FILE_MARKER="src/llvmir2hll/var_name_gen/var_name_gens/word_var_name_gen.cpp"

pass=0
fail=0
warn=0

pass_check() {
    printf '\033[32mPASS\033[0m  %s\n' "$1"
    pass=$((pass + 1))
}

fail_check() {
    printf '\033[31mFAIL\033[0m  %s\n' "$1"
    fail=$((fail + 1))
}

warn_check() {
    printf '\033[33mWARN\033[0m  %s\n' "$1"
    warn=$((warn + 1))
}

echo "RetDec doctor (Linux/macOS/WSL)"
echo "repo: $REPO_ROOT"
echo ""

# ── CMake ─────────────────────────────────────────────────────────────────────
if command -v cmake >/dev/null 2>&1; then
    ver="$(cmake --version | head -n1 | sed -E 's/.* ([0-9]+)\.([0-9]+).*/\1 \2/')"
    read -r maj min <<< "$ver"
    if [[ "$maj" -gt "$CMAKE_MIN_MAJOR" ]] || { [[ "$maj" -eq "$CMAKE_MIN_MAJOR" ]] && [[ "$min" -ge "$CMAKE_MIN_MINOR" ]]; }; then
        pass_check "cmake $(cmake --version | head -n1 | awk '{print $3}') (>= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR})"
    else
        fail_check "cmake $(cmake --version | head -n1 | awk '{print $3}') - need >= ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}"
    fi
else
    fail_check "cmake not on PATH - install CMake ${CMAKE_MIN_MAJOR}.${CMAKE_MIN_MINOR}+"
fi

# ── Large support files ───────────────────────────────────────────────────────
marker="$REPO_ROOT/$LARGE_FILE_MARKER"
if [[ -f "$marker" ]]; then
    pass_check "fetch-large-files marker present ($LARGE_FILE_MARKER)"
else
    fail_check "missing $LARGE_FILE_MARKER - run: bash scripts/fetch-large-files.sh"
fi

# ── Qt6 (optional hint for GUI presets) ───────────────────────────────────────
qt_ok=0
if command -v qmake6 >/dev/null 2>&1; then
    qt_ok=1
    pass_check "Qt6 hint: qmake6 on PATH ($(qmake6 -query QT_VERSION 2>/dev/null || echo unknown))"
elif command -v qmake >/dev/null 2>&1 && qmake -query QT_VERSION 2>/dev/null | grep -q '^6\.'; then
    qt_ok=1
    pass_check "Qt6 hint: qmake on PATH (Qt $(qmake -query QT_VERSION))"
elif pkg-config --exists Qt6Core 2>/dev/null; then
    qt_ok=1
    pass_check "Qt6 hint: pkg-config Qt6Core ($(pkg-config --modversion Qt6Core))"
elif [[ -n "${Qt6_DIR:-}" && -d "${Qt6_DIR}" ]]; then
    qt_ok=1
    pass_check "Qt6 hint: Qt6_DIR=${Qt6_DIR}"
fi
if [[ "$qt_ok" -eq 0 ]]; then
    warn_check "Qt6 not detected - GUI presets need qt6-base-dev (Linux) or Qt 6 MSVC kit (Windows)"
fi

# ── NSIS (Windows packaging only) ─────────────────────────────────────────────
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*)
        if command -v makensis >/dev/null 2>&1 || command -v makensis.exe >/dev/null 2>&1; then
            pass_check "NSIS makensis on PATH"
        else
            warn_check "makensis not found - optional for Windows installer (portable zip still works)"
        fi
        ;;
    *)
        warn_check "NSIS/makensis skipped (not a Windows shell)"
        ;;
esac

# ── git-lfs ───────────────────────────────────────────────────────────────────
if command -v git >/dev/null 2>&1; then
    if git lfs version >/dev/null 2>&1; then
        pass_check "git-lfs $(git lfs version 2>/dev/null | head -n1 | awk '{print $1, $3}')"
    else
        warn_check "git-lfs not available (optional unless you use legacy LFS objects)"
    fi
else
    fail_check "git not on PATH"
fi

# ── python3 ───────────────────────────────────────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    pass_check "python3 $(python3 --version 2>&1 | awk '{print $2}')"
elif command -v python >/dev/null 2>&1; then
    pass_check "python $(python --version 2>&1 | awk '{print $2}')"
else
    fail_check "python3 not on PATH - needed for validate_pipeline_json.py and ci-smoke tests"
fi

# ── licence files ─────────────────────────────────────────────────────────────
for f in LICENSE LICENSE-AGPL LICENSE-COMMERCIAL NOTICE; do
    if [ -f "${REPO_ROOT}/${f}" ]; then
        pass_check "${f} present"
    else
        fail_check "missing ${f} — run install-licence-files.sh"
    fi
done

# ── dependency release candidates ─────────────────────────────────────────────
if grep -qE '5\.0-rc|v4\.2\.0-rc' "${REPO_ROOT}/cmake/deps.cmake" 2>/dev/null; then
    fail_check "cmake/deps.cmake still pins a release-candidate dependency"
else
    pass_check "no RC dependency pins in cmake/deps.cmake"
fi

# ── CI workflows enabled ──────────────────────────────────────────────────────
workflow_has_trigger() {
	local wfpath="$1"
	shift
	local pattern
	for pattern in "$@"; do
		if grep -qE "^[[:space:]]*${pattern}:" "${wfpath}"; then
			return 0
		fi
	done
	return 1
}

if workflow_has_trigger "${REPO_ROOT}/.github/workflows/ci-smoke.yml" push; then
	pass_check "workflow ci-smoke.yml triggers on push"
else
	fail_check "workflow ci-smoke.yml missing push trigger"
fi

if workflow_has_trigger "${REPO_ROOT}/.github/workflows/ctest-linux.yml" push pull_request; then
	pass_check "workflow ctest-linux.yml triggers on push or pull_request"
else
	fail_check "workflow ctest-linux.yml missing push/pull_request trigger"
fi

if workflow_has_trigger "${REPO_ROOT}/.github/workflows/algorithm-recovery-nightly.yml" push schedule workflow_dispatch; then
	pass_check "workflow algorithm-recovery-nightly.yml has CI trigger"
else
	fail_check "workflow algorithm-recovery-nightly.yml missing schedule/workflow_dispatch"
fi

# ── commercial GPL exclusion ──────────────────────────────────────────────────
if grep -rq 'capstone2llvmirtool' "${REPO_ROOT}/packaging/" 2>/dev/null; then
    fail_check "capstone2llvmirtool found in packaging/ (commercial GPL exclusion)"
else
    pass_check "no capstone2llvmirtool in packaging manifests"
fi

# ── neural model SHA (optional) ───────────────────────────────────────────────
if [[ -n "${RETDEC_NEURAL_MODEL:-}" && -f "${RETDEC_NEURAL_MODEL}" ]]; then
    if [[ -n "${RETDEC_NEURAL_MODEL_SHA256:-}" ]]; then
        actual="$(sha256sum "${RETDEC_NEURAL_MODEL}" | awk '{print $1}')"
        if [[ "$actual" == "${RETDEC_NEURAL_MODEL_SHA256}" ]]; then
            pass_check "RETDEC_NEURAL_MODEL SHA256 matches"
        else
            fail_check "RETDEC_NEURAL_MODEL SHA256 mismatch (expected ${RETDEC_NEURAL_MODEL_SHA256})"
        fi
    else
        warn_check "RETDEC_NEURAL_MODEL set but RETDEC_NEURAL_MODEL_SHA256 unset"
    fi
fi

# ── algorithm recovery baseline ───────────────────────────────────────────────
baseline_ar="${REPO_ROOT}/results/baseline-algorithm-recovery.json"
if [[ -f "$baseline_ar" ]]; then
    pass_check "algorithm-recovery baseline present (results/baseline-algorithm-recovery.json)"
else
    warn_check "missing results/baseline-algorithm-recovery.json"
fi

# ── perl ──────────────────────────────────────────────────────────────────────
if command -v perl >/dev/null 2>&1; then
    pass_check "perl $(perl -e 'print $^V' 2>/dev/null || perl --version 2>/dev/null | sed -n '2p')"
else
    fail_check "perl not on PATH - required for bundled OpenSSL configure"
fi

echo ""
echo "Summary: $pass passed, $fail failed, $warn warnings"
if [[ "$fail" -gt 0 ]]; then
    exit 1
fi
exit 0
