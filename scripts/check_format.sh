#!/usr/bin/env bash
# Dry-run clang-format check on tracked sources under include/, src/, tests/.
# Exits 1 if any file would change. Install clang-format and run from repo root:
#   bash scripts/check_format.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if ! command -v clang-format &>/dev/null; then
  echo "check_format: clang-format not found in PATH" >&2
  exit 1
fi

FAILED=0
while IFS= read -r f; do
  [[ -z "$f" || ! -f "$f" ]] && continue
  if ! diff -q "$f" <(clang-format "$f") &>/dev/null; then
    echo "check_format: needs reformat: $f" >&2
    FAILED=1
  fi
done < <(git ls-files include/ src/ tests/ | grep -E '\.(cpp|h|hpp|cc|c|cu)$' || true)

if [[ $FAILED -ne 0 ]]; then
  echo "check_format: run clang-format -i on the files above (or: git ls-files include/ src/ tests/ | grep -E '\\.(cpp|h|hpp|cc|c|cu)$' | xargs clang-format -i)" >&2
  exit 1
fi

echo "check_format: OK"
