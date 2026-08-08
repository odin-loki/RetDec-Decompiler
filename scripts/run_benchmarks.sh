#!/usr/bin/env bash
# run_benchmarks.sh — DecompileBench + algorithm recovery (Part 6 / 16.2).
# Usage: bash scripts/run_benchmarks.sh [--compare TAG] [--gate] [--build-corpus] [--profile ci-core|full] [--fetch-stock]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHA="$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo local)"
OUT="${ROOT}/results/${SHA}.json"
COMPARE_TAG=""
RUN_GATE=0
BUILD_CORPUS=0
PROFILE="ci-core"
FETCH_STOCK=0

while [[ $# -gt 0 ]]; do
	case "$1" in
		--compare) COMPARE_TAG="$2"; shift 2 ;;
		--gate) RUN_GATE=1; shift ;;
		--build-corpus) BUILD_CORPUS=1; shift ;;
		--profile) PROFILE="$2"; shift 2 ;;
		--fetch-stock) FETCH_STOCK=1; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/results"

if [[ "${BUILD_CORPUS}" -eq 1 ]] || [[ ! -f "${ROOT}/tests/algorithm_recovery/corpus/manifest.json" ]]; then
	if command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1; then
		bash "${ROOT}/scripts/build_algorithm_corpus.sh" || true
	fi
fi

bash "${ROOT}/scripts/fetch_decompilebench_corpus.sh" --profile "${PROFILE}"

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

if [[ "${FETCH_STOCK}" -eq 1 ]]; then
	bash "${ROOT}/scripts/fetch_stock_retdec.sh" || true
fi
STOCK="${RETDEC_STOCK_DECOMPILER:-}"
if [[ -z "${STOCK}" ]]; then
	STOCK="$(find "${ROOT}/deps/stock-retdec" -name retdec-decompiler -type f 2>/dev/null | head -n1 || true)"
fi

LIMIT=""
[[ "${PROFILE}" == "ci-core" ]] && LIMIT="--limit 9"

if [[ -n "${DEC}" && -x "${DEC}" ]]; then
	BENCH_CMD=(python3 "${ROOT}/tests/decompilebench/runner.py"
		--decompiler "${DEC}"
		--corpus "${ROOT}/tests/decompilebench/corpus"
		--out "${ROOT}/results/decompilebench-tmp.json")
	[[ -n "${LIMIT}" ]] && BENCH_CMD+=(${LIMIT})
	if [[ -n "${STOCK}" && -x "${STOCK}" ]]; then
		BENCH_CMD+=(--baseline-decompiler "${STOCK}")
	fi
	"${BENCH_CMD[@]}" || true
fi

python3 - "${OUT}" "${COMPARE_TAG}" "${ROOT}" "${DEC}" "${PROFILE}" <<'PY'
import json, pathlib, subprocess, sys, time

out, compare, root, dec, profile = sys.argv[1:6]
root = pathlib.Path(root)
payload = {
    "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "git_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
    "compare_tag": compare or None,
    "profile": profile,
    "decompilebench": {"status": "skipped", "samples": []},
    "algorithm_recovery": {"status": "skipped"},
    "metrics": {
        "decompilebench": {
            "syntax_valid_rate": None,
            "recompile_success_rate": None,
            "coverage_equivalence_rate": None,
        },
        "algorithm_recovery": {"mean_f1": None, "mean_f1_raw": None},
    },
}

bench_tmp = root / "results/decompilebench-tmp.json"
if bench_tmp.is_file():
    bench = json.loads(bench_tmp.read_text(encoding="utf-8"))
    payload["decompilebench"] = bench
    summary = bench.get("summary", {})
    payload["metrics"]["decompilebench"] = {
        "syntax_valid_rate": summary.get("syntax_valid_rate"),
        "recompile_success_rate": summary.get("recompile_success_rate"),
        "coverage_equivalence_rate": summary.get("coverage_equivalence_rate"),
    }

corpus = root / "tests/algorithm_recovery/corpus"
gt = root / "tests/algorithm_recovery/ground_truth/corpus.json"
pred = root / "tests/algorithm_recovery/predictions/corpus.json"
if dec and pathlib.Path(dec).is_file() and gt.is_file() and corpus.is_dir():
    extract_args = [
        sys.executable, str(root / "scripts/extract_decompiler_predictions.py"),
        "--decompiler", dec, "--corpus", str(corpus),
        "--manifest", str(corpus / "manifest.json"),
        "--out", str(pred),
    ]
    if profile == "ci-core":
        extract_args.append("--ci-core")
    subprocess.run(extract_args, check=False)
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
            payload["metrics"]["algorithm_recovery"]["mean_f1"] = summary.get("mean_f1")
            payload["metrics"]["algorithm_recovery"]["mean_f1_raw"] = summary.get("mean_f1_raw")

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
