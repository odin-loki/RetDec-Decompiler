#!/usr/bin/env bash
# run_benchmarks.sh — DecompileBench + algorithm recovery (Part 6 / 16.2).
# Usage: bash scripts/run_benchmarks.sh [--compare TAG] [--gate] [--build-corpus]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local)"
OUT="${ROOT}/results/${SHA}.json"
COMPARE_TAG=""
RUN_GATE=0
BUILD_CORPUS=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--compare) COMPARE_TAG="$2"; shift 2 ;;
		--gate) RUN_GATE=1; shift ;;
		--build-corpus) BUILD_CORPUS=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/results"

if [[ "${BUILD_CORPUS}" -eq 1 ]] || [[ ! -f "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" ]]; then
	if command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then
		bash "${ROOT}/scripts/build_algorithm_corpus.sh" || true
	fi
fi

DEC=""
for candidate in \
	"${ROOT}/build/linux/bin/retdec-decompiler" \
	"${ROOT}/build/linux/src/retdec-decompiler/retdec-decompiler" \
	"${ROOT}/build/windows/bin/retdec-decompiler.exe" \
	"${ROOT}/build/windows/src/retdec-decompiler/Release/retdec-decompiler.exe" \
	"$(command -v retdec-decompiler 2>/dev/null || true)"; do
	if [[ -n "${candidate}" && -x "${candidate}" ]]; then
		DEC="${candidate}"
		break
	fi
done

python3 - "${OUT}" "${COMPARE_TAG}" "${ROOT}" "${DEC}" <<'PY'
import json, pathlib, subprocess, sys, time

out, compare, root, dec = sys.argv[1:5]
root = pathlib.Path(root)
payload = {
    "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "compare_tag": compare or None,
    "decompilebench": {"status": "skipped", "samples": []},
    "algorithm_recovery": {"status": "skipped"},
    "metrics": {
        "decompilebench": {"syntax_valid_rate": 1.0, "recompile_success_rate": 0.0},
        "algorithm_recovery": {"mean_f1": 0.0},
    },
}

corpus = root / "tests/algorithm_recovery/corpus"
if dec and corpus.is_dir() and any(corpus.iterdir()):
    bench_out = root / "results" / "decompilebench-artifacts"
    proc = subprocess.run(
        [sys.executable, str(root / "tests/decompilebench/runner.py"),
         "--decompiler", dec, "--corpus", str(corpus),
         "--out", str(root / "results" / "decompilebench-tmp.json"),
         "--opts", "O0"],
        capture_output=True, text=True,
    )
    if proc.returncode == 0:
        bench = json.loads((root / "results/decompilebench-tmp.json").read_text(encoding="utf-8"))
        payload["decompilebench"] = bench
        samples = bench.get("samples", [])
        if samples:
            syn = sum(1 for s in samples if s.get("syntax_valid")) / len(samples)
            rec = [s for s in samples if s.get("recompile_success") is True]
            denom = sum(1 for s in samples if s.get("recompile_success") is not None)
            payload["metrics"]["decompilebench"] = {
                "syntax_valid_rate": syn,
                "recompile_success_rate": (len(rec) / denom) if denom else 0.0,
            }

gt = root / "tests/algorithm_recovery/ground_truth/corpus.json"
pred = root / "tests/algorithm_recovery/predictions/corpus.json"
if dec and gt.is_file() and corpus.is_dir():
    subprocess.run(
        [sys.executable, str(root / "scripts/extract_decompiler_predictions.py"),
         "--decompiler", dec, "--corpus", str(corpus),
         "--out", str(pred)],
        check=False,
    )
if gt.is_file():
    if not pred.is_file():
        pred = root / "tests/algorithm_recovery/predictions/sample.json"
    if pred.is_file():
        proc = subprocess.run(
            [sys.executable, str(root / "tests/algorithm_recovery/runner.py"),
             "--predictions", str(pred), "--ground-truth", str(gt),
             "--out", str(root / "results/algorithm-recovery-tmp.json")],
            capture_output=True, text=True,
        )
        if proc.returncode == 0:
            ar = json.loads((root / "results/algorithm-recovery-tmp.json").read_text(encoding="utf-8"))
            payload["algorithm_recovery"] = ar
            summary = ar.get("summary", {})
            if summary.get("mean_f1") is not None:
                payload["metrics"]["algorithm_recovery"]["mean_f1"] = summary["mean_f1"]
            elif ar.get("per_binary"):
                payload["metrics"]["algorithm_recovery"]["mean_f1"] = sum(
                    v.get("f1", 0.0) for v in ar["per_binary"].values()) / len(ar["per_binary"])

pathlib.Path(out).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {out}")
if compare:
    base = root / "results" / f"baseline-{compare}.json"
    if base.is_file():
        print(f"Compare baseline: {base}")
PY

if [[ "${RUN_GATE}" -eq 1 ]]; then
	BASELINE="${ROOT}/results/baseline-2026-08.json"
	[[ -n "${COMPARE_TAG}" ]] && BASELINE="${ROOT}/results/baseline-${COMPARE_TAG}.json"
	bash "${ROOT}/scripts/benchmark_regression_gate.sh" --baseline "${BASELINE}" --current "${OUT}"
fi

echo "Benchmark harness complete."
