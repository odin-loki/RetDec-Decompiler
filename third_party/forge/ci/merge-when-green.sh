#!/usr/bin/env bash
# Merge a PR only when every CI check has passed. `gh pr checks` exits 0 only when all
# checks passed; 8 means some are still pending, any other non-zero means a failure.
# Usage: ci/merge-when-green.sh <pr-number> [extra gh pr merge args]
set -euo pipefail
pr="${1:?usage: $0 <pr-number> [gh pr merge args]}"; shift
rc=0; gh pr checks "$pr" >/dev/null 2>&1 || rc=$?
if [ "$rc" -ne 0 ]; then
  gh pr checks "$pr" || true
  echo "PR #$pr: not all CI checks passed (gh pr checks exit $rc); refusing to merge." >&2
  exit 1
fi
# Also refuse if the PR has no checks at all (nothing reported yet).
n=$(gh pr checks "$pr" --json state --jq 'length')
if [ "$n" -eq 0 ]; then echo "PR #$pr: no CI checks reported; refusing to merge." >&2; exit 1; fi
bad=$(gh pr checks "$pr" --json state --jq '[.[] | select(.state != "SUCCESS" and .state != "SKIPPED")] | length')
if [ "$bad" -ne 0 ]; then echo "PR #$pr: $bad check(s) not successful; refusing to merge." >&2; exit 1; fi
gh pr merge "$pr" "$@"
