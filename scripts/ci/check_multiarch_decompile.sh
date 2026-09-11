#!/usr/bin/env bash
# ARCH-01 — decompile the same sources built for ARM, ARM64, MIPS and PowerPC,
# and report the success rate per architecture.
#
# Why this exists
# ---------------
# src/capstone2llvmir translates five architectures. Every end-to-end gate in
# this repository measures one: all 216 binaries in
# tests/algorithm_recovery/corpus are x86-64, so CC-01 (216/216), DET-01 (72
# binaries) and the F1 recovery gate -- the three numbers this branch publishes
# -- say nothing at all about the other four. C2L-01 covers their instruction
# semantics. Nothing covered what the decompiler does with a whole binary.
#
# Measurement first, floor second
# -------------------------------
# Without --min-rate this prints the rates and exits 0. With it, the rate is a
# floor and the script fails below it. That split is taken from CC-01, and for
# the same reason it was right there: nobody here knows today's rate, and a
# floor picked before the first measurement is either so low it gates nothing
# or so high it reports a regression that was never a state the code was in.
#
# The floors are per architecture, not one number over all four. An overall
# rate lets a healthy ARM64 carry a MIPS that decompiles nothing: with 36
# binaries each, ARM64 at 1.00 and MIPS at 0.00 is an overall 0.75, which reads
# like "mostly working" and is not.
#
# What counts as a success: the decompiler exits 0 within the timeout and
# writes a non-empty .c. That is the first question to ask of an architecture
# nothing has ever run, and it is deliberately weaker than CC-01's -- whether
# the emitted C compiles is the next floor to raise, not this one.
#
# Usage: bash scripts/ci/check_multiarch_decompile.sh --decompiler PATH
#            [--corpus DIR] [--limit N] [--timeout SECONDS]
#            [--min-rate FLOAT] [--arch-min "arm64=0.9 mips=0.5"]
#            [--save-failures DIR] [--self-test]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC=""
CORPUS=""
LIMIT=0
TIMEOUT=180
MIN_RATE=""
ARCH_MIN=""
SAVE_FAILURES=""
SELF_TEST=0

while [ $# -gt 0 ]; do
	case "$1" in
		--decompiler)     DEC="$2"; shift 2 ;;
		--corpus)         CORPUS="$2"; shift 2 ;;
		--limit)          LIMIT="$2"; shift 2 ;;
		--timeout)        TIMEOUT="$2"; shift 2 ;;
		--min-rate)       MIN_RATE="$2"; shift 2 ;;
		--arch-min)       ARCH_MIN="$2"; shift 2 ;;
		--save-failures)  SAVE_FAILURES="$2"; shift 2 ;;
		--self-test)      SELF_TEST=1; shift ;;
		*) echo "ARCH-01: unknown option: $1" >&2; exit 2 ;;
	esac
done

# A floor that cannot be parsed must not read as "no floor". Same reason
# CC-01 rejects `--min-rate banana`: that is how a gate gets turned off by a
# typo and nobody notices, because the run stays green.
if [ -n "${MIN_RATE}" ]; then
	case "${MIN_RATE}" in
		0|1|0.[0-9]*|1.0*) : ;;
		*) echo "ARCH-01: --min-rate must be a number between 0 and 1, got '${MIN_RATE}'" >&2
		   exit 2 ;;
	esac
fi

run_check() {
	local dec="$1" corpus="$2"
	local work; work="$(mktemp -d -t arch01-XXXXXX)"
	# shellcheck disable=SC2064
	trap "rm -rf '${work}'" RETURN

	local bins=()
	while IFS= read -r b; do bins+=("$b"); done < <(
		find "${corpus}" -maxdepth 1 -type f ! -name '.*' ! -name '*.json' \
			! -name '*.md' | sort)
	if [ "${#bins[@]}" = 0 ]; then
		echo "ARCH-01: FAIL corpus is empty: ${corpus}" >&2
		return 1
	fi
	if [ "${LIMIT}" -gt 0 ] && [ "${#bins[@]}" -gt "${LIMIT}" ]; then
		local trimmed=()
		local i=0
		for b in "${bins[@]}"; do
			[ "${i}" -lt "${LIMIT}" ] || break
			trimmed+=("$b"); i=$((i + 1))
		done
		bins=("${trimmed[@]}")
	fi

	: > "${work}/results"
	for bin in "${bins[@]}"; do
		local name arch out log
		name="$(basename "${bin}")"
		# The builder names every file <stem>-<arch>-gcc-<opt>.
		arch="$(echo "${name}" | sed -n 's/.*-\(arm64\|arm\|mips\|powerpc\)-gcc-O[0-9s]*$/\1/p')"
		[ -n "${arch}" ] || arch="unknown"
		out="${work}/${name}.c"
		log="${work}/${name}.log"
		local rc=0
		timeout --kill-after=15 "${TIMEOUT}" \
			"${dec}" -o "${out}" "${bin}" >"${log}" 2>&1 || rc=$?
		if [ "${rc}" = 0 ] && [ -s "${out}" ]; then
			echo "${arch} ok ${name}" >> "${work}/results"
		else
			echo "${arch} fail ${name} rc=${rc}" >> "${work}/results"
			if [ -n "${SAVE_FAILURES}" ]; then
				mkdir -p "${SAVE_FAILURES}"
				cp "${log}" "${SAVE_FAILURES}/${name}.log" 2>/dev/null || true
			fi
		fi
	done

	# Report every architecture that has binaries, in a fixed order, so a
	# vanished architecture shows as a missing line rather than as a smaller
	# total nobody reads.
	local bad=0
	local total_ok=0 total_all=0
	for arch in arm arm64 mips powerpc unknown; do
		local n_all n_ok
		n_all="$(grep -c "^${arch} " "${work}/results" || true)"
		[ "${n_all}" != 0 ] || continue
		n_ok="$(grep -c "^${arch} ok " "${work}/results" || true)"
		total_ok=$((total_ok + n_ok)); total_all=$((total_all + n_all))
		local rate; rate="$(awk -v a="${n_ok}" -v b="${n_all}" 'BEGIN{printf "%.4f", b?a/b:0}')"
		printf 'ARCH-01: %-8s %3d/%-3d  %s\n' "${arch}" "${n_ok}" "${n_all}" "${rate}"

		local floor=""
		for pair in ${ARCH_MIN}; do
			[ "${pair%%=*}" = "${arch}" ] && floor="${pair##*=}"
		done
		[ -n "${floor}" ] || floor="${MIN_RATE}"
		if [ -n "${floor}" ]; then
			if awk -v r="${rate}" -v f="${floor}" 'BEGIN{exit !(r < f)}'; then
				echo "ARCH-01: FAIL ${arch}: ${rate} is below the floor ${floor}" >&2
				bad=1
			fi
		fi
	done

	local overall; overall="$(awk -v a="${total_ok}" -v b="${total_all}" 'BEGIN{printf "%.4f", b?a/b:0}')"
	printf 'ARCH-01: %-8s %3d/%-3d  %s\n' "overall" "${total_ok}" "${total_all}" "${overall}"

	if [ "${bad}" != 0 ]; then
		grep ' fail ' "${work}/results" | head -10 >&2
		return 1
	fi
	# No floor given: this is a measurement and says so rather than implying
	# that a rate of anything at all was accepted.
	if [ -z "${MIN_RATE}" ] && [ -z "${ARCH_MIN}" ]; then
		echo "ARCH-01: measurement only -- no --min-rate or --arch-min given, nothing was gated"
	else
		echo "ARCH-01: OK every architecture met its floor"
	fi
	return 0
}

if [ "${SELF_TEST}" = 1 ]; then
	# Fake decompilers and a fake corpus, so the arithmetic, the per-arch split
	# and the floors are all exercised without the real thing.
	T="$(mktemp -d -t arch01-self-XXXXXX)"
	trap 'rm -rf "${T}"' EXIT
	mkdir -p "${T}/corpus" "${T}/bin"
	for arch in arm arm64 mips powerpc; do
		for i in 1 2 3 4; do
			printf 'fake\n' > "${T}/corpus/prog${i}-${arch}-gcc-O0"
		done
	done

	cat > "${T}/bin/good" <<'EOF'
#!/usr/bin/env bash
out=""; while [ $# -gt 0 ]; do [ "$1" = "-o" ] && { out="$2"; shift 2; continue; }; shift; done
printf 'int main(void){return 0;}\n' > "$out"
EOF
	# Succeeds everywhere except MIPS, which is the shape this check exists to
	# make visible: three architectures fine, one dead, overall 0.75.
	cat > "${T}/bin/no_mips" <<'EOF'
#!/usr/bin/env bash
out=""; in=""; while [ $# -gt 0 ]; do
  case "$1" in -o) out="$2"; shift 2;; *) in="$1"; shift;; esac; done
case "$in" in *-mips-*) exit 1;; esac
printf 'int main(void){return 0;}\n' > "$out"
EOF
	# Exits 0 but writes nothing: a pass that produced no output is a failure,
	# and a check that only looked at the exit status would call it green.
	cat > "${T}/bin/empty" <<'EOF'
#!/usr/bin/env bash
out=""; while [ $# -gt 0 ]; do [ "$1" = "-o" ] && { out="$2"; shift 2; continue; }; shift; done
: > "$out"
EOF
	chmod +x "${T}/bin/"*

	fails=0
	expect() { # want-exit, description, then args
		local want="$1" desc="$2"; shift 2
		local out rc=0
		out="$(run_check "$@" 2>&1)" || rc=$?
		if [ "${rc}" != "${want}" ]; then
			echo "self-test: ${desc}: expected exit ${want}, got ${rc}" >&2
			printf '%s\n' "${out}" >&2
			fails=$((fails + 1))
		fi
		printf '%s' "${out}"
	}

	echo "ARCH-01: self-test"
	out="$(MIN_RATE=1.0 ARCH_MIN="" expect 0 "a decompiler that works everywhere meets a floor of 1.0" "${T}/bin/good" "${T}/corpus")"
	printf '%s' "${out}" | grep -q 'overall   16/16 *1.0000' \
		|| { echo "self-test: expected overall 16/16 1.0000, got:" >&2; printf '%s\n' "${out}" >&2; fails=$((fails+1)); }

	out="$(MIN_RATE="" ARCH_MIN="" expect 0 "no floor is a measurement, not a pass" "${T}/bin/no_mips" "${T}/corpus")"
	printf '%s' "${out}" | grep -q 'measurement only' \
		|| { echo "self-test: a floorless run must say it gated nothing" >&2; fails=$((fails+1)); }
	printf '%s' "${out}" | grep -q 'mips *0/4 *0.0000' \
		|| { echo "self-test: expected mips 0/4, got:" >&2; printf '%s\n' "${out}" >&2; fails=$((fails+1)); }

	# The point of the per-architecture split: overall 0.75 passes a 0.5 floor
	# while MIPS is dead, and only the per-arch floor catches it.
	out="$(MIN_RATE=0.5 ARCH_MIN="" expect 1 "an overall floor of 0.5 still fails when one architecture is 0" "${T}/bin/no_mips" "${T}/corpus")"
	printf '%s' "${out}" | grep -q 'FAIL mips' \
		|| { echo "self-test: expected the failure to name mips" >&2; fails=$((fails+1)); }

	# These two are the ones that prove --arch-min is read at all. The first
	# version of this self-test set MIN_RATE="" alongside a per-arch floor, so
	# ignoring --arch-min entirely left no floor and the case passed either
	# way -- it could not fail, which is the defect this file exists to catch.
	# Each of these now flips its outcome if the per-arch lookup is dropped.

	# On its own, with no overall floor: a per-arch floor must still gate.
	out="$(MIN_RATE="" ARCH_MIN="mips=1.0" expect 1 "a per-arch floor gates with no overall floor set" "${T}/bin/no_mips" "${T}/corpus")"
	printf '%s' "${out}" | grep -q 'FAIL mips' \
		|| { echo "self-test: expected the per-arch floor to name mips" >&2; fails=$((fails+1)); }

	# And it must override the overall floor downward, not just upward: mips
	# is 0.0 and the overall floor is 0.5, so only the per-arch entry can
	# let this pass.
	out="$(MIN_RATE=0.5 ARCH_MIN="mips=0.0" expect 0 "a per-arch floor overrides the overall floor for that architecture" "${T}/bin/no_mips" "${T}/corpus")"

	out="$(MIN_RATE=0.5 ARCH_MIN="" expect 1 "exit 0 with an empty output is a failure" "${T}/bin/empty" "${T}/corpus")"
	printf '%s' "${out}" | grep -q 'overall    0/16 *0.0000' \
		|| { echo "self-test: expected overall 0/16, got:" >&2; printf '%s\n' "${out}" >&2; fails=$((fails+1)); }

	if [ "${fails}" != 0 ]; then
		echo "ARCH-01: self-test FAILED (${fails})" >&2
		exit 1
	fi
	echo "ARCH-01: self-test OK"
	exit 0
fi

if [ -z "${DEC}" ] || [ ! -x "${DEC}" ]; then
	echo "ARCH-01: retdec-decompiler not found: ${DEC:-<empty>}" >&2
	exit 2
fi
if [ -z "${CORPUS}" ]; then
	CORPUS="${ROOT}/tests/algorithm_recovery/corpus-multiarch"
fi
if [ ! -d "${CORPUS}" ]; then
	echo "ARCH-01: corpus directory not found: ${CORPUS}" >&2
	echo "  build it with: bash scripts/build_multiarch_corpus.sh --out ${CORPUS}" >&2
	exit 2
fi

run_check "${DEC}" "${CORPUS}"
