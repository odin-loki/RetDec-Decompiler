#!/usr/bin/env bash
# A/B detector-stage cost on one ci-core binary (audit C9).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEC="${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler"
BIN="${ROOT}/tests/algorithm_recovery/corpus/binary_search-gcc-O0"
OUT="${ROOT}/results/detector-stage-ab.json"
WORKDIR="${ROOT}/build/linux/ab-detectors"
mkdir -p "${WORKDIR}"
run_one() {
	local label="$1"
	shift
	local outc="${WORKDIR}/${label}.c"
	local t0 t1
	t0="$(date +%s.%N)"
	env RETDEC_PROFILE_JSON=1 "$@" "${DEC}" "${BIN}" --output "${outc}"
	t1="$(date +%s.%N)"
	python3 - "${outc}.profile.json" "${label}" "${t0}" "${t1}" <<'PY'
import json, sys
path, label, t0, t1 = sys.argv[1:5]
wall = float(t1) - float(t0)
prof = json.load(open(path, encoding="utf-8"))
stages = {s.get("name"): s.get("total_ms") for s in prof.get("stages", []) if isinstance(s, dict)}
print(json.dumps({"label": label, "wall_s": round(wall, 3), "detectors_ms": stages.get("analysis.detectors"), "stages": stages}))
PY
}
echo "=== default detectors ==="
run_one default
echo "=== skip semantic ==="
run_one skip RETDEC_SKIP_SEMANTIC_RECOVERY=1
python3 - "${WORKDIR}" "${OUT}" <<'PY'
import json, pathlib, sys
wd, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
rows = []
for label in ("default", "skip"):
    prof = json.loads((wd / f"{label}.c.profile.json").read_text(encoding="utf-8"))
    stages = {s.get("name"): s.get("total_ms") for s in prof.get("stages", []) if isinstance(s, dict)}
    rows.append({"label": label, "detectors_ms": stages.get("analysis.detectors"), "stages": stages})
dest.parent.mkdir(parents=True, exist_ok=True)
dest.write_text(json.dumps({"sample": "binary_search-gcc-O0", "runs": rows}, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {dest}")
PY
