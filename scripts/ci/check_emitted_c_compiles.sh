#!/usr/bin/env bash
# CC-01 — the C this decompiler emits has to compile.
#
# Why this exists
# ---------------
# Nothing in this repository checks that.  DET-01 checks that two runs emit the
# *same* C, CACHE-05 that the cache does not change it, the algorithm-recovery
# gate that the detections are right -- every one of them reads the output as
# text and none of them hands it to a compiler.  So the emitted `.c` could stop
# being C at all and every gate would stay green.
#
# It did.  `README.md` reports the default `.c` recompiling 0/216 while the
# `--buildable` sidecar reports 216/216, and that sidecar exists to work around
# the gap.  `docs/internal/UNFIXED_AUDIT_FINDINGS.md` names three separate
# whole-translation-unit failures in the C writer -- a goto whose label was
# moved onto a discarded statement, a label at the end of a compound statement,
# a declaration deleted out from under its uses.  Each one is enough on its own
# to hold every binary at zero, and none of them is visible to anything that
# runs today.
#
# What it does
# ------------
# Decompiles each selected binary and hands the result to `cc -fsyntax-only`.
# That is a weaker question than "does it link and run" and a much cheaper one,
# and it is the question the three known defects fail: an undeclared label, an
# undeclared identifier and a label at the end of a block are all syntax- or
# semantics-level errors the front end alone reports.
#
# Reporting versus gating
# -----------------------
# Without `--min-rate` this is a *measurement*: it prints the rate and exits 0.
# With it, the rate is a floor and the script fails below it.
#
# The split is deliberate.  Nobody here knows today's rate -- the README's
# 0/216 predates the branch and nothing has re-measured it -- and a floor
# invented rather than measured is either vacuous or flaky.  So the first CI run
# reports, that number becomes the floor in a follow-up commit, and from then on
# the gate can fail.  A gate that cannot fail is worse than no gate; so is one
# whose threshold nobody has justified.
#
# Usage:
#   bash scripts/ci/check_emitted_c_compiles.sh --decompiler PATH --corpus DIR \
#        [--limit N] [--timeout SECONDS] [--min-rate FLOAT] [--errors N]
#   bash scripts/ci/check_emitted_c_compiles.sh --self-test
set -euo pipefail

SELF="${BASH_SOURCE[0]}"
ROOT="$(cd "$(dirname "${SELF}")/../.." && pwd)"

# ── self-test ────────────────────────────────────────────────────────────────
#
# Does this script actually fail when the emitted C does not compile?  Checked
# against stub decompilers that behave in the five ways that matter: output that
# compiles, output that does not, output that is empty, no output at all, and a
# crash.  Each case asserts both the reported rate and the exit status, because
# a rate that is right while the exit status is not is still a gate that cannot
# fail.
if [[ "${1:-}" == "--self-test" ]]; then
	set +e
	T="$(mktemp -d)"
	trap 'rm -rf "${T}"' EXIT
	mkdir -p "${T}/corpus" "${T}/bin"
	for n in prog_a-gcc-O0 prog_b-clang-O2 prog_c-gcc-O3 prog_d-clang-O0; do
		printf 'fake %s\n' "${n}" > "${T}/corpus/${n}"
		chmod +x "${T}/corpus/${n}"
	done

	# Emits a translation unit that compiles.
	cat > "${T}/bin/good" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
cat > "${out}" <<'C'
#include <stdint.h>
int64_t f(void) { int64_t v = 1; return v; }
C
FAKE

	# Emits the exact shape the goto/label defect produces: a goto whose label
	# is nowhere in the file.
	cat > "${T}/bin/undeclared_label" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
cat > "${out}" <<'C'
int f(int c) { if (c) goto lab_4006f0; return 0; }
C
FAKE

	# ...and the shape the deleted-declaration defect produces.
	cat > "${T}/bin/undeclared_ident" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
cat > "${out}" <<'C'
int f(void) { v7 = 3; return v7; }
C
FAKE

	# One binary out of four fails to compile; the rest are fine.
	cat > "${T}/bin/mostly_good" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
if [[ "$(basename "${bin}")" == "prog_c-gcc-O3" ]]; then
	printf 'int f(void) { return undeclared_thing; }\n' > "${out}"
else
	printf 'int f(void) { return 0; }\n' > "${out}"
fi
FAKE

	# Writes an empty file: not C, and not a compile success either.
	cat > "${T}/bin/empty" <<'FAKE'
#!/usr/bin/env bash
out=""; bin=""
while [[ $# -gt 0 ]]; do case "$1" in -o) out="$2"; shift 2;; *) bin="$1"; shift;; esac; done
: > "${out}"
FAKE

	# Writes nothing at all and fails, the way a crash does.
	cat > "${T}/bin/crash" <<'FAKE'
#!/usr/bin/env bash
echo "fake-decompiler: Assertion \`this cannot happen' failed."
exit 1
FAKE
	chmod +x "${T}/bin/"*

	fails=0
	# $1 want-exit, $2 want-rate-substring, $3 description, rest: extra args
	expect() {
		local want="$1" wantrate="$2" desc="$3"; shift 3
		local out got
		out="$(bash "${SELF}" --corpus "${T}/corpus" "$@" 2>&1)"
		got=$?
		if [[ "${got}" -ne "${want}" ]]; then
			echo "self-test: ${desc}: expected exit ${want}, got ${got}" >&2
			printf '%s\n' "${out}" >&2
			fails=$(( fails + 1 ))
			return
		fi
		if [[ -n "${wantrate}" ]] && ! printf '%s' "${out}" | grep -qF -- "${wantrate}"; then
			echo "self-test: ${desc}: expected output to contain '${wantrate}'" >&2
			printf '%s\n' "${out}" >&2
			fails=$(( fails + 1 ))
		fi
	}

	# 1. Everything compiles: reports 4/4 and passes, gated or not.
	expect 0 "4/4" "output that compiles reports 4/4" \
		--decompiler "${T}/bin/good"
	expect 0 "4/4" "...and clears a floor of 1.0" \
		--decompiler "${T}/bin/good" --min-rate 1.0

	# 2. Nothing compiles.  Report mode still exits 0 -- it is a measurement --
	#    but the rate has to say 0/4, and any floor above zero must fail.
	expect 0 "0/4" "an undeclared label reports 0/4" \
		--decompiler "${T}/bin/undeclared_label"
	expect 1 "0/4" "...and fails a floor of 0.5" \
		--decompiler "${T}/bin/undeclared_label" --min-rate 0.5
	expect 1 "0/4" "an undeclared identifier fails a floor of 0.5" \
		--decompiler "${T}/bin/undeclared_ident" --min-rate 0.5

	# 3. A partial regression is the case a floor exists to catch.
	expect 0 "3/4" "one bad file out of four reports 3/4" \
		--decompiler "${T}/bin/mostly_good"
	expect 1 "3/4" "...and fails a floor of 1.0" \
		--decompiler "${T}/bin/mostly_good" --min-rate 1.0
	expect 0 "3/4" "...and clears a floor of 0.75" \
		--decompiler "${T}/bin/mostly_good" --min-rate 0.75

	# 4. An empty file is not a compile success.  cc accepts an empty
	#    translation unit in some modes, so this is asserted rather than
	#    assumed: a decompiler that emits nothing must not score.
	expect 1 "0/4" "an empty file does not count as compiling" \
		--decompiler "${T}/bin/empty" --min-rate 0.5

	# 5. No output at all, and a crash, are counted as failures rather than
	#    skipped -- a binary the decompiler cannot emit C for has not emitted
	#    C that compiles.
	expect 1 "0/4" "a crash counts as a failure" \
		--decompiler "${T}/bin/crash" --min-rate 0.5

	# 6. The failure has to say what the compiler said, or it is a rate with
	#    no lead to follow.
	expect 1 "lab_4006f0" "the report quotes the compiler's own error" \
		--decompiler "${T}/bin/undeclared_label" --min-rate 0.5

	# 7. A floor is only a floor if the script refuses one it cannot read.
	expect 2 "" "a non-numeric --min-rate is rejected" \
		--decompiler "${T}/bin/good" --min-rate banana

	cd "${ROOT}"
	if [[ "${fails}" -ne 0 ]]; then
		echo "check_emitted_c_compiles self-test: ${fails} case(s) failed" >&2
		exit 1
	fi
	echo "check_emitted_c_compiles self-test OK"
	exit 0
fi

cd "${ROOT}"

DEC=""
CORPUS=""
LIMIT=0
TIMEOUT=120
MIN_RATE=""
ERRORS=5
while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--corpus) CORPUS="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--min-rate) MIN_RATE="$2"; shift 2 ;;
		--errors) ERRORS="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 2 ;;
	esac
done

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	echo "retdec-decompiler not found: ${DEC:-<empty>}" >&2
	exit 2
fi
if [[ -z "${CORPUS}" || ! -d "${CORPUS}" ]]; then
	echo "corpus directory not found: ${CORPUS:-<empty>}" >&2
	exit 2
fi
# A floor that cannot be read is not a floor.  Rejected rather than defaulted,
# because silently treating `--min-rate banana` as "no floor" turns a gate off
# in a way the log does not show.
if [[ -n "${MIN_RATE}" ]] && ! [[ "${MIN_RATE}" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
	echo "CC-01: --min-rate must be a number between 0 and 1, got '${MIN_RATE}'" >&2
	exit 2
fi

CC="${CC:-cc}"
if ! command -v "${CC}" >/dev/null 2>&1; then
	echo "CC-01: no C compiler (${CC}) -- cannot check whether the output compiles" >&2
	exit 2
fi

# Sorted, so the selection is the same on every run.
mapfile -t ALL < <(find "${CORPUS}" -maxdepth 1 -type f -perm -u+x ! -name '*.json' \
	! -name '*.c' ! -name '*.txt' -print | sort)
if [[ ${#ALL[@]} -eq 0 ]]; then
	echo "CC-01: no binaries under ${CORPUS} -- nothing to check" >&2
	exit 2
fi

# Same sampling rule as DET-01: hash the name rather than take every N-th of a
# sorted list, because the corpus is named <program>-<compiler>-O<level> and
# sorts into groups of six.  Hashing spreads the sample over compilers and
# optimisation levels and gives the same sample every run.
BINS=()
if [[ "${LIMIT}" -gt 0 && ${#ALL[@]} -gt "${LIMIT}" ]]; then
	mapfile -t BINS < <(
		for b in "${ALL[@]}"; do
			printf '%s %s\n' "$(basename "${b}" | md5sum | cut -c1-16)" "${b}"
		done | sort | head -n "${LIMIT}" | cut -d' ' -f2-
	)
else
	BINS=("${ALL[@]}")
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

OK=0
BAD=0
NOOUT=0
FAILED_NAMES=()
ERRLOG="${WORK}/errors.txt"
: > "${ERRLOG}"

for bin in "${BINS[@]}"; do
	name="$(basename "${bin}")"
	out="${WORK}/${name}.c"
	log="${WORK}/${name}.decompile.log"
	rm -f "${out}"
	RETDEC_INCREMENTAL_CACHE=0 \
		timeout --kill-after=15 "${TIMEOUT}" \
		"${DEC}" -o "${out}" "${bin}" >"${log}" 2>&1 || true

	# An empty file is not a translation unit that compiles.  cc accepts an
	# empty input in some modes, so this is checked before the compiler runs
	# rather than left to it.
	if [[ ! -s "${out}" ]]; then
		NOOUT=$(( NOOUT + 1 ))
		BAD=$(( BAD + 1 ))
		FAILED_NAMES+=("${name} (no output)")
		{
			printf '=== %s: the decompiler produced no C ===\n' "${name}"
			tail -n 5 "${log}" 2>/dev/null
		} >> "${ERRLOG}"
		continue
	fi

	cerr="${WORK}/${name}.cc.log"
	if "${CC}" -fsyntax-only -std=c11 -w "${out}" >"${cerr}" 2>&1; then
		OK=$(( OK + 1 ))
	else
		BAD=$(( BAD + 1 ))
		FAILED_NAMES+=("${name}")
		{
			printf '=== %s ===\n' "${name}"
			grep -m 3 -E 'error:' "${cerr}" 2>/dev/null || head -n 3 "${cerr}"
		} >> "${ERRLOG}"
	fi
done

TOTAL=$(( OK + BAD ))
RATE="$(awk -v o="${OK}" -v t="${TOTAL}" 'BEGIN { if (t == 0) print "0.0000"; else printf "%.4f", o / t }')"

echo "CC-01: ${OK}/${TOTAL} emitted C files compile (rate ${RATE}, ${NOOUT} with no output)"

if [[ "${BAD}" -gt 0 ]]; then
	echo "CC-01: files that did not compile:"
	printf '  %s\n' "${FAILED_NAMES[@]}" | head -n 20
	echo "CC-01: what the compiler said (first ${ERRORS}):"
	head -n "$(( ERRORS * 4 ))" "${ERRLOG}" | sed 's/^/  /'
fi

if [[ -z "${MIN_RATE}" ]]; then
	echo "CC-01: measurement only -- no --min-rate given, so this cannot fail."
	echo "CC-01: set --min-rate to ${RATE} to hold this, once it is believed."
	exit 0
fi

if awk -v r="${RATE}" -v m="${MIN_RATE}" 'BEGIN { exit !(r < m) }'; then
	echo "CC-01: FAIL rate ${RATE} is below the floor ${MIN_RATE}" >&2
	exit 1
fi

echo "CC-01: OK rate ${RATE} is at or above the floor ${MIN_RATE}"
