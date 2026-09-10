#!/usr/bin/env bash
# Verify that the algorithm-recovery regression gate can actually fail a run.
#
# Why this exists
# ---------------
# The gate has three layers and only two of them were checked:
#
#   1. algorithm_recovery_regression_gate.py decides whether the drop against
#      the baseline is tolerable. tests/algorithm_recovery/test_regression_gate.py
#      covers this and it is correct.
#   2. algorithm_recovery_regression_gate.sh execs that. Nothing covered it.
#   3. run_algorithm_recovery_ci.sh calls the shell wrapper -- and called it
#      with `|| true`, so the verdict was printed and discarded. A run could
#      print "REGRESSION: mean_f1 dropped by 0.10" and stay green.
#
# Layer 1 passing says nothing about the gate being able to fail, which is the
# only property that matters. This checks layers 2 and 3.
#
# Usage: bash scripts/ci/check_recovery_gate_wiring.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/baseline.json" <<'JSON'
{
  "metrics": { "ci_core": { "mean_f1": 0.5000, "min_decompiled": 9 } },
  "thresholds": { "mean_f1_drop_max": 0.05, "decompiled_drop_max": 10 }
}
JSON

cat > "${WORK}/regressed.json" <<'JSON'
{ "summary": { "decompiled": 9, "mean_f1": 0.3000 } }
JSON

cat > "${WORK}/held.json" <<'JSON'
{ "summary": { "decompiled": 9, "mean_f1": 0.4900 } }
JSON

status=0

# ── Layer 2: the shell wrapper propagates the python verdict ─────────────────

if bash scripts/algorithm_recovery_regression_gate.sh \
	--baseline "${WORK}/baseline.json" --current "${WORK}/regressed.json" \
	> "${WORK}/regressed.log" 2>&1
then
	echo "GATE-01: FAIL the regression gate returned success on a 0.20 F1 drop" >&2
	cat "${WORK}/regressed.log" >&2
	status=1
else
	if ! grep -q "REGRESSION" "${WORK}/regressed.log"; then
		echo "GATE-01: FAIL the gate failed but did not say why" >&2
		cat "${WORK}/regressed.log" >&2
		status=1
	fi
fi

if ! bash scripts/algorithm_recovery_regression_gate.sh \
	--baseline "${WORK}/baseline.json" --current "${WORK}/held.json" \
	> "${WORK}/held.log" 2>&1
then
	echo "GATE-01: FAIL the regression gate rejected a drop inside its tolerance" >&2
	cat "${WORK}/held.log" >&2
	status=1
fi

# ── Layer 3: the CI driver does not discard that verdict ─────────────────────
#
# A textual check, because running the driver needs a built decompiler and a
# corpus. It is narrow on purpose: it looks for the regression gate being
# invoked with its exit status suppressed, which is the one way layer 2 stops
# mattering.

DRIVER="scripts/run_algorithm_recovery_ci.sh"
if [[ ! -f "${DRIVER}" ]]; then
	echo "GATE-01: FAIL ${DRIVER} does not exist" >&2
	exit 1
fi

if ! grep -q "algorithm_recovery_regression_gate.sh" "${DRIVER}"; then
	echo "GATE-01: FAIL ${DRIVER} does not run the regression gate at all" >&2
	exit 1
fi

# The invocation and its continuation lines, up to the first line that is not
# a continuation.
invocation="$(awk '
	/algorithm_recovery_regression_gate\.sh/ { inv = 1 }
	inv { print; if ($0 !~ /\\$/) exit }
' "${DRIVER}")"

if printf '%s' "${invocation}" | grep -qE '\|\|[[:space:]]*(true|:)'; then
	echo "GATE-01: FAIL ${DRIVER} discards the regression gate's exit status:" >&2
	printf '%s\n' "${invocation}" >&2
	echo "         The gate prints its verdict and the run stays green." >&2
	status=1
fi

if [[ "${status}" -ne 0 ]]; then
	exit 1
fi

echo "GATE-01: OK the regression gate fails a real drop, holds a tolerable one, and its verdict reaches the run"
