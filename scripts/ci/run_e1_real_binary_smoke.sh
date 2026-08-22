#!/usr/bin/env bash
# run_e1_real_binary_smoke.sh — name-blind E1 smoke on 3 real gcc-O0 ELFs.
#
# Decompile binary_search / bubblesort / memcpy_loop from
# tests/algorithm_recovery/corpus (not decompilebench WSL symlinks).
# Read labels from the decompiler .config.json sidecar
# (functions[].semanticDetections). Do not map to F1, do not use
# filename/stem fallback, do not invent labels from the basename.
#
# Also decompile a sha256-prefix copy of each binary (B6). Labels must
# be identical; a named-only detection is filename coupling.
#
# Exit 0 even if some detections are empty (honest).
# Exit 1 only if the decompiler crashes/times out, or rename changes labels.
#
# Usage:
#   bash scripts/ci/run_e1_real_binary_smoke.sh [--decompiler PATH] [--timeout S]
#
# Env:
#   E1_SMOKE_TIMEOUT   per-decompile seconds, default 180

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
CORPUS="${ROOT}/tests/algorithm_recovery/corpus"
OUTDIR="${ROOT}/build/linux/e1-smoke"
RESULTS="${ROOT}/results/e1-real-binary-smoke.json"
TIMEOUT="${E1_SMOKE_TIMEOUT:-180}"

NAMES=(
	binary_search-gcc-O0
	bubblesort-gcc-O0
	memcpy_loop-gcc-O0
)

while [[ $# -gt 0 ]]; do
	case "$1" in
		--decompiler) DEC="$2"; shift 2 ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

if [[ ! -x "${DEC}" ]]; then
	echo "e1-real-binary-smoke: missing decompiler: ${DEC}" >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "e1-real-binary-smoke: python3 not on PATH" >&2
	exit 1
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

mkdir -p "${OUTDIR}/hashed" "${ROOT}/results"

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

echo "e1-real-binary-smoke: decompiler=${DEC}"
echo "e1-real-binary-smoke: corpus=${CORPUS}"
echo "e1-real-binary-smoke: outdir=${OUTDIR}"
echo "e1-real-binary-smoke: timeout=${TIMEOUT}s"

CRASHED=0
declare -a JOBS=()

for name in "${NAMES[@]}"; do
	src="${CORPUS}/${name}"
	if [[ ! -f "${src}" ]]; then
		echo "e1-real-binary-smoke: missing corpus ELF: ${src}" >&2
		exit 1
	fi

	digest="$(sha256_of "${src}")"
	prefix="${digest:0:16}"
	hashed="${OUTDIR}/hashed/${prefix}"
	cp -f "${src}" "${hashed}"

	out_named="${OUTDIR}/${name}.c"
	out_hash="${OUTDIR}/hashed/${prefix}.c"
	log_named="${OUTDIR}/${name}.log"
	log_hash="${OUTDIR}/hashed/${prefix}.log"

	echo "--- named ${name} ---"
	named_rc=0
	if ! run_decompile "${src}" "${out_named}" "${log_named}"; then
		named_rc=$?
		echo "e1-real-binary-smoke: named decompile failed rc=${named_rc} for ${name}" >&2
		CRASHED=1
	fi

	echo "--- hashed ${name} -> ${prefix} ---"
	hash_rc=0
	if ! run_decompile "${hashed}" "${out_hash}" "${log_hash}"; then
		hash_rc=$?
		echo "e1-real-binary-smoke: hashed decompile failed rc=${hash_rc} for ${name}" >&2
		CRASHED=1
	fi

	JOBS+=("${name}|${prefix}|${out_named}|${out_hash}|${named_rc}|${hash_rc}")
done

# Parse functions[].semanticDetections from each sidecar. Empty is allowed.
# Fail only if named vs hashed kind+label sets differ (filename coupling).
COUPLING=0
python3 - "${RESULTS}" "${DEC}" "${CORPUS}" "${OUTDIR}" "${CRASHED}" "${JOBS[@]}" <<'PY'
from __future__ import annotations

import json
import sys
from pathlib import Path


def find_config(out_c: Path) -> Path | None:
    cands = [
        out_c.parent / (out_c.stem + ".config.json"),
        out_c.parent / (out_c.name + ".config.json"),
        Path(str(out_c) + ".config.json"),
    ]
    for p in cands:
        if p.is_file():
            return p
    return None


def detections_from_config(cfg_path: Path | None) -> tuple[list[dict], list[str], str | None]:
    if cfg_path is None:
        return [], [], "missing .config.json sidecar"
    try:
        cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [], [], f"unreadable config: {exc}"
    rows: list[dict] = []
    labels: set[str] = set()
    for fn in cfg.get("functions") or []:
        if not isinstance(fn, dict):
            continue
        for det in fn.get("semanticDetections") or []:
            if not isinstance(det, dict):
                continue
            kind = str(det.get("kind") or "").strip()
            label = str(det.get("label") or "").strip()
            if not kind and not label:
                continue
            rec = {"kind": kind, "label": label}
            conf = det.get("confidence")
            if isinstance(conf, (int, float)):
                rec["confidence"] = float(conf)
            rows.append(rec)
            labels.add(f"{kind.lower()}:{label.lower()}")
    rows.sort(key=lambda r: (r.get("kind", ""), r.get("label", "")))
    return rows, sorted(labels), None


results_path = Path(sys.argv[1])
decompiler = sys.argv[2]
corpus = sys.argv[3]
outdir = sys.argv[4]
crashed = sys.argv[5] == "1"
jobs = sys.argv[6:]

binaries = []
coupling = False
for job in jobs:
    name, prefix, named_c, hash_c, named_rc, hash_rc = job.split("|")
    named_cfg = find_config(Path(named_c))
    hash_cfg = find_config(Path(hash_c))
    named_dets, named_labels, named_err = detections_from_config(named_cfg)
    hash_dets, hash_labels, hash_err = detections_from_config(hash_cfg)
    named_ok = named_rc == "0" and named_err is None
    hash_ok = hash_rc == "0" and hash_err is None
    if named_rc != "0" or hash_rc != "0":
        crashed = True
    if named_ok and hash_ok and named_err is None and hash_err is None:
        identical = named_labels == hash_labels
    elif not named_ok or not hash_ok:
        identical = False
        crashed = True
    else:
        identical = named_labels == hash_labels
    if named_ok and hash_ok and not identical:
        coupling = True
        print(
            f"FAIL: filename coupling on {name}: "
            f"named={named_labels} hashed={hash_labels}",
            file=sys.stderr,
        )
    empty = named_ok and not named_labels
    binaries.append(
        {
            "name": name,
            "sha256_prefix": prefix,
            "labels_identical": identical,
            "empty": empty,
            "named": {
                "ok": named_ok,
                "rc": int(named_rc),
                "config": str(named_cfg) if named_cfg else None,
                "error": named_err,
                "detections": named_dets,
                "labels": named_labels,
            },
            "hashed": {
                "ok": hash_ok,
                "rc": int(hash_rc),
                "config": str(hash_cfg) if hash_cfg else None,
                "error": hash_err,
                "detections": hash_dets,
                "labels": hash_labels,
            },
        }
    )
    print(f"  {name} named={named_labels} hashed={hash_labels} identical={identical} empty={empty}")

ok = (not crashed) and (not coupling)
payload = {
    "ok": ok,
    "filename_coupling": coupling,
    "crashed": crashed,
    "empty_ok": True,
    "note": (
        "Detections read from functions[].semanticDetections in .config.json. "
        "Empty detections are honest and do not fail. No F1 gate. "
        "No filename-to-label mapping."
    ),
    "corpus": corpus,
    "decompiler": decompiler,
    "outdir": outdir,
    "binaries": binaries,
}
results_path.parent.mkdir(parents=True, exist_ok=True)
results_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"e1-real-binary-smoke: wrote {results_path}")
if coupling:
    sys.exit(2)
if crashed:
    sys.exit(3)
sys.exit(0)
PY
py_rc=$?

if [[ "${py_rc}" -eq 2 || "${COUPLING}" -eq 1 ]]; then
	echo "e1-real-binary-smoke: FAIL — rename changed labels (filename coupling)"
	exit 1
fi
if [[ "${py_rc}" -eq 3 || "${CRASHED}" -ne 0 ]]; then
	echo "e1-real-binary-smoke: FAIL — decompiler crash or missing config sidecar"
	exit 1
fi
if [[ "${py_rc}" -ne 0 ]]; then
	echo "e1-real-binary-smoke: FAIL — report script rc=${py_rc}"
	exit 1
fi

echo "e1-real-binary-smoke: OK (empty detections allowed; no F1 gate)"
exit 0
