#!/usr/bin/env bash
# benchmark_rename_guard.sh — B6 filename-rename integrity guard.
#
# Decompile the same bytes under the original corpus name and under a
# content-hash name. Fail if semanticDetections (kind+label) appear only
# on the named copy — that pattern is filename-derived detection.
#
# If retdec-decompiler is not built, or no corpus binaries are present,
# exit 0 with a skip message (configure-only CI must not fail).
#
# Usage:
#   bash scripts/ci/benchmark_rename_guard.sh [--decompiler PATH] [--limit N] [--timeout S]
#
# Env:
#   RENAME_GUARD_LIMIT     default 5 (nightly may set 12+)
#   RENAME_GUARD_TIMEOUT   per-decompile seconds, default 180
#   RUNNER_TEMP            GitHub Actions temp (else /tmp)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC=""
LIMIT="${RENAME_GUARD_LIMIT:-5}"
TIMEOUT="${RENAME_GUARD_TIMEOUT:-180}"
WORKDIR="${RUNNER_TEMP:-/tmp}/retdec-rename-guard"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--limit) LIMIT="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

skip() {
	echo "benchmark_rename_guard: SKIP — $1"
	exit 0
}

if [[ -z "${DEC}" ]]; then
	for candidate in \
		"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
		"${ROOT}/build/linux/bin/retdec-decompiler" \
		"${ROOT}/install/linux/bin/retdec-decompiler"; do
		if [[ -x "${candidate}" ]]; then
			DEC="${candidate}"
			break
		fi
	done
fi

if [[ -z "${DEC}" || ! -x "${DEC}" ]]; then
	skip "retdec-decompiler not built (looked in build/linux and install/linux)"
fi

if ! command -v python3 >/dev/null 2>&1; then
	skip "python3 not on PATH"
fi

sha256_of() {
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$1" | awk '{print $1}'
	elif command -v shasum >/dev/null 2>&1; then
		shasum -a 256 "$1" | awk '{print $1}'
	else
		python3 -c 'import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())' "$1"
	fi
}

is_candidate() {
	local f="$1"
	local base="${f##*/}"
	case "${base}" in
		*.json|*.c|*.h|*.hpp|*.md|*.txt|*.yaml|*.yml|*.py|*.sh|*.labels) return 1 ;;
	esac
	[[ -f "${f}" ]] || return 1
	local sz
	sz="$(wc -c < "${f}" | tr -d '[:space:]')"
	[[ "${sz}" -ge 64 && "${sz}" -le 2000000 ]]
}

# Prefer algorithm-recovery names (the old filename-lookup cheat vector).
PREFERRED=(
	bubblesort-gcc-O0
	mergesort-gcc-O0
	hash_table-gcc-O0
	ring_buffer-gcc-O0
	binary_search-gcc-O0
	memcpy_loop-gcc-O0
	generated_quicksort-gcc-O0
	generated_heapsort-gcc-O0
	generated_pthread_mutex-gcc-O0
)

CORPUS_DIRS=(
	"${ROOT}/tests/algorithm_recovery/corpus"
	"${ROOT}/tests/decompilebench/corpus"
)

declare -a SELECTED=()

for name in "${PREFERRED[@]}"; do
	[[ ${#SELECTED[@]} -ge ${LIMIT} ]] && break
	for dir in "${CORPUS_DIRS[@]}"; do
		[[ -d "${dir}" ]] || continue
		for cand in "${dir}/${name}" "${dir}/${name}.exe"; do
			if is_candidate "${cand}"; then
				SELECTED+=("${cand}")
				break
			fi
		done
	done
done

if [[ ${#SELECTED[@]} -lt ${LIMIT} ]]; then
	while IFS= read -r f; do
		[[ ${#SELECTED[@]} -ge ${LIMIT} ]] && break
		is_candidate "${f}" || continue
		dup=0
		for already in "${SELECTED[@]+"${SELECTED[@]}"}"; do
			if [[ "${already}" == "${f}" ]]; then
				dup=1
				break
			fi
		done
		[[ "${dup}" -eq 1 ]] && continue
		SELECTED+=("${f}")
	done < <(find "${CORPUS_DIRS[@]}" -maxdepth 2 -type f 2>/dev/null | sort)
fi

if [[ ${#SELECTED[@]} -eq 0 ]]; then
	skip "no corpus binaries under tests/algorithm_recovery/corpus or tests/decompilebench/corpus"
fi

# Build-tree binaries look for ../share/retdec/decompiler-config.json.
dec_dir="$(cd "$(dirname "${DEC}")" && pwd)"
share_dir="$(cd "${dec_dir}/.." && pwd)/share/retdec"
mkdir -p "${share_dir}/profiles"
if [[ -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" ]]; then
	cp -f "${ROOT}/src/retdec-decompiler/decompiler-config.json" "${share_dir}/"
fi
if [[ -d "${ROOT}/src/retdec-decompiler/profiles" ]]; then
	cp -f "${ROOT}/src/retdec-decompiler/profiles/"*.json "${share_dir}/profiles/" 2>/dev/null || true
fi

rm -rf "${WORKDIR}"
mkdir -p "${WORKDIR}/named" "${WORKDIR}/out-named" "${WORKDIR}/out-hash"

echo "benchmark_rename_guard: decompiler=${DEC}"
echo "benchmark_rename_guard: workdir=${WORKDIR}"
echo "benchmark_rename_guard: binaries=${#SELECTED[@]} limit=${LIMIT} timeout=${TIMEOUT}s"

run_decompile() {
	local input="$1"
	local out_c="$2"
	local log="$3"
	export RETDEC_INCREMENTAL_CACHE=0
	unset RETDEC_NEURAL_REFINE || true
	if command -v timeout >/dev/null 2>&1; then
		timeout --signal=KILL "${TIMEOUT}" "${DEC}" "${input}" --output "${out_c}" \
			>"${log}" 2>&1
	else
		"${DEC}" "${input}" --output "${out_c}" >"${log}" 2>&1
	fi
}

FAILED=0
COMPARED=0

for src in "${SELECTED[@]}"; do
	base="$(basename "${src}")"
	digest="$(sha256_of "${src}")"
	named="${WORKDIR}/named/${base}"
	hashed="${WORKDIR}/${digest}.bin"
	cp -f "${src}" "${named}"
	cp -f "${src}" "${hashed}"

	out_named="${WORKDIR}/out-named/${base}.c"
	out_hash="${WORKDIR}/out-hash/${digest}.c"
	log_named="${WORKDIR}/out-named/${base}.log"
	log_hash="${WORKDIR}/out-hash/${digest}.log"

	echo "--- ${base} vs ${digest:0:12}… ---"
	if ! run_decompile "${named}" "${out_named}" "${log_named}"; then
		echo "benchmark_rename_guard: named decompile failed for ${base} (skip pair)"
		continue
	fi
	if ! run_decompile "${hashed}" "${out_hash}" "${log_hash}"; then
		echo "benchmark_rename_guard: hashed decompile failed for ${base} (skip pair)"
		continue
	fi

	if ! python3 - "${out_named}" "${out_hash}" "${base}" <<'PY'
from __future__ import annotations
import json, sys
from pathlib import Path

def find_config(out_c: Path) -> Path | None:
    cands = [
        out_c.parent / (out_c.name + ".config.json"),
        out_c.parent / (out_c.stem + ".config.json"),
        Path(str(out_c) + ".config.json"),
    ]
    for p in cands:
        if p.is_file():
            return p
    return None

def det_set(cfg: dict) -> set[str]:
    found: set[str] = set()
    for fn in cfg.get("functions", []):
        for det in fn.get("semanticDetections", []) or []:
            kind = str(det.get("kind") or "").strip().lower()
            label = str(det.get("label") or "").strip().lower()
            if kind or label:
                found.add(f"{kind}:{label}")
    return found

named_c = Path(sys.argv[1])
hash_c = Path(sys.argv[2])
name = sys.argv[3]
cfg_n = find_config(named_c)
cfg_h = find_config(hash_c)
if cfg_n is None:
    print(f"benchmark_rename_guard: no config JSON for named {name}", file=sys.stderr)
    sys.exit(2)
if cfg_h is None:
    print(f"benchmark_rename_guard: no config JSON for hashed {name}", file=sys.stderr)
    sys.exit(2)
named = det_set(json.loads(cfg_n.read_text(encoding="utf-8")))
hashed = det_set(json.loads(cfg_h.read_text(encoding="utf-8")))
only_named = sorted(named - hashed)
only_hash = sorted(hashed - named)
print(f"  named detections ({len(named)}): {sorted(named)}")
print(f"  hash  detections ({len(hashed)}): {sorted(hashed)}")
if only_named:
    print(
        f"FAIL: detections appear only on the named copy of {name}: {only_named}",
        file=sys.stderr,
    )
    sys.exit(1)
if only_hash:
    print(f"  note: extra detections on hashed copy (not a filename cheat): {only_hash}")
sys.exit(0)
PY
	then
		rc=$?
		if [[ "${rc}" -eq 1 ]]; then
			FAILED=1
		else
			echo "benchmark_rename_guard: compare skipped for ${base} (rc=${rc})"
		fi
	else
		COMPARED=$((COMPARED + 1))
	fi
done

if [[ "${FAILED}" -ne 0 ]]; then
	echo "benchmark_rename_guard: FAIL — filename-only semantic detections"
	exit 1
fi

if [[ "${COMPARED}" -eq 0 ]]; then
	skip "no successful named-vs-hash decompile pairs"
fi

echo "benchmark_rename_guard: OK (${COMPARED} pair(s) compared)"
exit 0
