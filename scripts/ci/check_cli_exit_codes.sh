#!/usr/bin/env bash
# CLI-01 — the exit code says whether the decompilation worked.
#
# Why this exists
# ---------------
# docs/internal/UNFIXED_AUDIT_FINDINGS.md section 4 recorded four ways this
# tree reported success for work it had not done:
#
#   * retdec::decompile() is declared bool and returned EXIT_SUCCESS, which is
#     0, which is false -- so the library told every caller that every
#     successful run had failed, and parallelBatchDecompile() handed that
#     straight out as its std::vector<bool>.
#   * llvmir2hll's getOutputStream() failure path exited 0 having written
#     nothing, so `-o /nonexistent/dir/out.c` succeeded silently.
#   * the CLI had no unknown-option diagnostic at all: a malformed command line
#     printed help to stdout, exited 0, and took the unrecognised option as the
#     input filename.
#
# All four are fixed in the source. None of them was checked anywhere, which is
# how they got in: an exit code nothing reads is an exit code nothing keeps
# right. This reads them.
#
# The success case is here for the same reason the self-test is: a gate that
# only asserts failures passes on a decompiler that fails at everything.
#
# Usage: bash scripts/ci/check_cli_exit_codes.sh --decompiler PATH [--corpus DIR]
#        bash scripts/ci/check_cli_exit_codes.sh --self-test
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

fail=0
note() { printf 'CLI-01: %s\n' "$*"; }
bad()  { printf 'CLI-01: FAIL %s\n' "$*" >&2; fail=1; }

# Runs "$@" and reports its exit status, without set -e taking the script down.
status_of() {
	local rc=0
	"$@" >/dev/null 2>&1 || rc=$?
	printf '%s' "${rc}"
}

# ── the checks, against whichever decompiler is given ────────────────────────
run_checks() {
	local dec="$1" corpus="$2"

	local rc
	rc="$(status_of "${dec}" --no-such-option-at-all)"
	if [[ "${rc}" -eq 0 ]]; then
		bad "an unknown option exited 0; a malformed command line is being taken as input"
	else
		note "unknown option: exit ${rc}"
	fi

	# Not `find ... | sort | head -n1`. head closes the pipe after one line, sort
	# takes EPIPE and exits 2, and `set -o pipefail` makes that the status of
	# the command substitution -- so `set -e` kills the check. Whether it
	# happens at all depends on whether sort's output fits the pipe buffer
	# before head exits, which makes it a race rather than a failure: it passed
	# here on a one-file corpus, passed in ctest-linux run 281, and failed in
	# run 280 on the same 216 files.
	#
	# Reading the whole listing and taking the first element has no early
	# close, so there is nothing to race.
	local input=""
	if [[ -d "${corpus}" ]]; then
		local -a candidates=()
		while IFS= read -r f; do
			candidates+=("$f")
		done < <(find "${corpus}" -maxdepth 1 -type f ! -name '*.json' ! -name '*.md' 2>/dev/null \
			| LC_ALL=C sort)
		input="${candidates[0]:-}"
	fi
	if [[ -z "${input}" ]]; then
		bad "no input binary under ${corpus}; the success and output-path checks cannot run"
		return
	fi

	rc="$(status_of "${dec}" -o /nonexistent-directory-for-cli-01/out.c "${input}")"
	if [[ "${rc}" -eq 0 ]]; then
		bad "an unopenable output path exited 0; nothing was written and it said so"
	else
		note "unopenable output path: exit ${rc}"
	fi

	# The control. Without it every assertion above is satisfied by a
	# decompiler that cannot do anything at all.
	local work out
	work="$(mktemp -d)"
	trap 'rm -rf "${work}"' RETURN
	out="${work}/out.c"
	rc="$(status_of "${dec}" -o "${out}" "${input}")"
	if [[ "${rc}" -ne 0 ]]; then
		bad "decompiling ${input##*/} exited ${rc}; the failures above prove nothing"
	elif [[ ! -s "${out}" ]]; then
		bad "decompiling ${input##*/} exited 0 and wrote nothing to ${out}"
	else
		note "$(basename "${input}"): exit 0, $(wc -c < "${out}") bytes"
	fi
}

# ── self-test ───────────────────────────────────────────────────────────────
#
# Two stub decompilers: one behaving as the source does now, one behaving as it
# did before the four fixes. The check must pass on the first and fail on the
# second, and the second is the point -- it is the shape this exists to catch.
if [[ "${1:-}" == "--self-test" ]]; then
	work="$(mktemp -d)"
	trap 'rm -rf "${work}"' EXIT
	# Two thousand files on purpose, and the number is the point.
	#
	# The first version of this self-test wrote one. `find | sort | head -n1`
	# picked the input then, and with one line sort never had a reader close
	# early on it -- so the check passed here and failed in ctest-linux run 280
	# on the real 216-file corpus, where sort took EPIPE, exited 2, and
	# `set -o pipefail` handed that to `set -e`. Run 281 passed on the same
	# code: whether it happens depends on whether sort's output fits the pipe
	# buffer before head exits, so it was a race and not a failure.
	#
	# Two thousand paths is past the 64 KB buffer, which makes it deterministic:
	# measured three times out of three against the old line.
	mkdir -p "${work}/corpus"
	for i in $(seq 1 2000); do
		printf 'not really a binary' > "${work}/corpus/fake-${i}.elf"
	done
	printf '{}' > "${work}/corpus/skipme.json"
	printf '# notes' > "${work}/corpus/skipme.md"

	cat > "${work}/good" <<'STUB'
#!/usr/bin/env bash
out=""
for a in "$@"; do
	case "$prev" in -o) out="$a" ;; esac
	prev="$a"
	case "$a" in --no-such-option-at-all) echo "unknown option: $a" >&2; exit 1 ;; esac
done
if [ -n "$out" ]; then
	d="$(dirname "$out")"
	[ -d "$d" ] || { echo "cannot open $out" >&2; exit 1; }
	printf 'int main(void) { return 0; }\n' > "$out"
fi
exit 0
STUB

	cat > "${work}/broken" <<'STUB'
#!/usr/bin/env bash
# Exits 0 whatever happens, and writes nothing. The state section 4 recorded.
exit 0
STUB
	chmod +x "${work}/good" "${work}/broken"

	rc=0
	bash "${BASH_SOURCE[0]}" --decompiler "${work}/good" --corpus "${work}/corpus" \
		> "${work}/good.log" 2>&1 || rc=$?
	if [[ "${rc}" -ne 0 ]]; then
		echo "self-test: the check failed on a decompiler that behaves correctly" >&2
		cat "${work}/good.log" >&2
		exit 1
	fi

	rc=0
	bash "${BASH_SOURCE[0]}" --decompiler "${work}/broken" --corpus "${work}/corpus" \
		> "${work}/broken.log" 2>&1 || rc=$?
	if [[ "${rc}" -eq 0 ]]; then
		echo "self-test: the check PASSED on a decompiler that exits 0 for everything" >&2
		echo "self-test: that is the exact defect it exists to catch" >&2
		cat "${work}/broken.log" >&2
		exit 1
	fi
	for want in "an unknown option exited 0" "an unopenable output path exited 0" "wrote nothing"; do
		if ! grep -qF "${want}" "${work}/broken.log"; then
			echo "self-test: the broken stub was rejected without naming '${want}'" >&2
			cat "${work}/broken.log" >&2
			exit 1
		fi
	done

	echo "CLI-01: self-test OK (passes a correct decompiler, fails one that exits 0 for everything)"
	exit 0
fi

# ── arguments ───────────────────────────────────────────────────────────────
DECOMPILER=""
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DECOMPILER="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

if [[ -z "${DECOMPILER}" ]] || [[ ! -x "${DECOMPILER}" ]]; then
	echo "CLI-01: --decompiler must name an executable (got '${DECOMPILER}')" >&2
	exit 1
fi

run_checks "${DECOMPILER}" "${CORPUS}"

if [[ "${fail}" -ne 0 ]]; then
	echo "CLI-01: FAIL the exit code does not say whether the run worked" >&2
	exit 1
fi

echo "CLI-01: OK"
