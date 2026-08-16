#!/usr/bin/env bash
# Dry-run clang-format check on C/C++ under include/, src/, tests/.
# Locally: all tracked sources. In GitHub Actions: only files changed by the
# push/PR (the historical tree is not clang-format-clean).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-format &>/dev/null; then
  echo "check_format: clang-format not found in PATH" >&2
  exit 1
fi

# Full-tree format is not green on this LLVM-8-era codebase. In CI, only
# check C/C++ files touched by the current push/PR so new work stays clean.
list_sources() {
  if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
    local before="${GITHUB_EVENT_BEFORE:-}"
    if [[ -z "${before}" || "${before}" =~ ^0+$ ]]; then
      before="$(git rev-parse HEAD^ 2>/dev/null || true)"
    fi
    if [[ -n "${before}" ]] && git cat-file -e "${before}^{commit}" 2>/dev/null; then
      git diff --name-only "${before}" HEAD -- include/ src/ tests/
      return
    fi
    if [[ -n "${GITHUB_BASE_REF:-}" ]]; then
      git fetch --depth=1 origin "${GITHUB_BASE_REF}" >/dev/null 2>&1 || true
      git diff --name-only "origin/${GITHUB_BASE_REF}...HEAD" -- include/ src/ tests/
      return
    fi
    # Shallow clone with no usable base: do not scan the historic tree.
    return
  fi
  git ls-files include/ src/ tests/
}

FAILED=0
CHECKED=0
while IFS= read -r f; do
  [[ -z "$f" || ! -f "$f" ]] && continue
  [[ "$f" =~ \.(cpp|h|hpp|cc|c|cu)$ ]] || continue
  CHECKED=$((CHECKED + 1))
  if ! diff -q "$f" <(clang-format "$f") &>/dev/null; then
    echo "check_format: needs reformat: $f" >&2
    FAILED=1
  fi
done < <(list_sources)

echo "check_format: checked ${CHECKED} file(s)"

if [[ $FAILED -ne 0 ]]; then
  echo "check_format: run clang-format -i on the files above (or: git ls-files include/ src/ tests/ | grep -E '\\.(cpp|h|hpp|cc|c|cu)$' | xargs clang-format -i)" >&2
  exit 1
fi

echo "check_format: OK"
