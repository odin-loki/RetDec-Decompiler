#!/usr/bin/env bash
# scripts/fetch-large-files.sh
# ---------------------------------------------------------------------------
# Download the large source data files that are deliberately not committed
# to keep this repo small (see .gitignore). Required before the first build.
#
# Run from the repo root (bash works even when this file is not executable):
#   bash scripts/fetch-large-files.sh               # default, only fetch missing
#   ./scripts/fetch-large-files.sh                  # same, after: chmod +x scripts/*.sh
#   bash scripts/fetch-large-files.sh --force       # overwrite even if present
#   bash scripts/fetch-large-files.sh --base-url URL   # alternate mirror
# ---------------------------------------------------------------------------
set -euo pipefail

BASE_URL="https://raw.githubusercontent.com/avast/retdec/master"
FORCE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --base-url) BASE_URL="$2"; shift 2 ;;
        --force)    FORCE=1;  shift ;;
        -h|--help)
            grep -E '^#' "$0" | sed -E 's/^#\s?//'
            exit 0 ;;
        *) echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

files=(
    "support/types/windows.json"
    "support/types/windrivers.json"
    "support/types/linux.json"
    "support/yara_patterns/signsrch/signsrch.yara"
    "support/yara_patterns/tools/pe/x86/packers.yara"
    "src/llvmir2hll/var_name_gen/var_name_gens/word_var_name_gen.cpp"
    "support/ordinals/x86/mfc100.ord"   "support/ordinals/x86/mfc100d.ord"
    "support/ordinals/x86/mfc100u.ord"  "support/ordinals/x86/mfc100ud.ord"
    "support/ordinals/x86/mfc110.ord"   "support/ordinals/x86/mfc110d.ord"
    "support/ordinals/x86/mfc110u.ord"  "support/ordinals/x86/mfc110ud.ord"
)

downloaded=0; skipped=0; failed=0
for rel in "${files[@]}"; do
    dst="$repo/$rel"
    mkdir -p "$(dirname "$dst")"
    if [[ -f "$dst" && "$FORCE" -eq 0 ]]; then
        printf '\033[2mskip   %s\033[0m\n' "$rel"
        skipped=$((skipped+1))
        continue
    fi
    printf 'fetch  %s ' "$rel"
    url="$BASE_URL/$rel"
    if curl --fail --silent --location --output "$dst" "$url"; then
        sz=$(wc -c < "$dst")
        printf '\033[32m(%s KiB)\033[0m\n' "$((sz / 1024))"
        downloaded=$((downloaded+1))
    else
        printf '\033[31mFAILED\033[0m\n'
        failed=$((failed+1))
    fi
done

echo
echo "Done: downloaded $downloaded, skipped $skipped, failed $failed."
[[ "$failed" -eq 0 ]]
