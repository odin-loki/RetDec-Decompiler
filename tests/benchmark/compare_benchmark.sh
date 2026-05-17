#!/usr/bin/env bash
# compare_benchmark.sh — Compare a new score against a saved baseline.
#
# Usage:
#   bash compare_benchmark.sh <new_score.json> [baseline.json]
#
#   If baseline.json is omitted, defaults to
#     $(dirname $0)/baseline.json
#
# Exit codes:
#   0  — all checks are same or better than baseline
#   1  — one or more checks regressed
#   2  — usage / file-not-found error

set -uo pipefail

NEW_JSON="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASELINE_JSON="${2:-$SCRIPT_DIR/baseline.json}"

if [ -z "$NEW_JSON" ]; then
    echo "Usage: $0 <new_score.json> [baseline.json]" >&2
    exit 2
fi

for f in "$NEW_JSON" "$BASELINE_JSON"; do
    [ -f "$f" ] || { echo "File not found: $f" >&2; exit 2; }
done

# ─── tiny JSON extractor (pure bash, no jq required) ─────────────────────────
get_total() {
    # Extract "total_score": N from a score JSON
    grep '"total_score"' "$1" | grep -oE '[0-9]+' | head -1
}

get_check_score() {
    # $1=file $2=check_name → score value (first match only)
    local name="$2"
    awk -v n="$name" '
        /"name"/ && index($0, "\"" n "\"") { found=1 }
        found && /"score"/ { match($0, /[0-9]+/); print substr($0, RSTART, RLENGTH); found=0; exit }
    ' "$1"
}

get_check_names() {
    # Extract "name": "CheckName" values — the check names are CamelCase
    grep '"name"' "$1" | grep -oE '"[A-Z][A-Za-z]+"' | tr -d '"'
}

# ─── comparison ──────────────────────────────────────────────────────────────
baseline_total=$(get_total "$BASELINE_JSON")
new_total=$(get_total "$NEW_JSON")
total_delta=$(( new_total - baseline_total ))

echo ""
echo "======================================================="
echo " RetDec Benchmark Comparison"
echo " Baseline : $BASELINE_JSON"
echo " New      : $NEW_JSON"
echo "======================================================="
printf "  %-28s  %6s  %6s  %6s\n" "Check" "Base" "New" "Delta"
echo "  ─────────────────────────────────────────────────────"

any_regression=0
while IFS= read -r name; do
    base_sc=$(get_check_score "$BASELINE_JSON" "$name")
    new_sc=$(get_check_score "$NEW_JSON" "$name")
    base_sc="${base_sc:-0}"
    new_sc="${new_sc:-0}"
    delta=$(( new_sc - base_sc ))
    flag=""
    if   [ "$delta" -lt 0 ]; then flag="  *** REGRESSION ***"; any_regression=1
    elif [ "$delta" -gt 0 ]; then flag="  (+improved)"
    fi
    printf "  %-28s  %6d  %6d  %+6d%s\n" "$name" "$base_sc" "$new_sc" "$delta" "$flag"
done < <(get_check_names "$BASELINE_JSON")

echo "  ─────────────────────────────────────────────────────"
printf "  %-28s  %6d  %6d  %+6d\n" "TOTAL" "$baseline_total" "$new_total" "$total_delta"
echo "======================================================="
echo ""

if [ "$any_regression" -eq 1 ]; then
    echo "RESULT: REGRESSIONS DETECTED — investigate before merging."
    exit 1
else
    echo "RESULT: OK — no regressions (total delta: ${total_delta:+$total_delta}${total_delta:- 0})."
    exit 0
fi
