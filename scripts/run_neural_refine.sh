#!/usr/bin/env bash
# Decompile one binary with Qwen 3.5 9B (llama.cpp-native GGUF).
# Compile gate stays on unless you export RETDEC_NEURAL_SKIP_COMPILE_GATE=1.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEC="${RETDEC_DECOMPILER:-$ROOT/build/linux/src/retdec-decompiler/retdec-decompiler}"
MODEL="${RETDEC_NEURAL_MODEL:-}"
if [[ -z "${MODEL}" ]]; then
	for cand in \
		"$ROOT/models/Qwen3.5-9B-Q4_K_M.gguf" \
		"$ROOT/models/Qwen3.5-9B-Q4_K_M.unsloth.gguf" \
		"$ROOT/models/Qwen3.5-9B-Instruct-Q4_K_M.gguf"
	do
		if [[ -f "$cand" ]]; then
			MODEL="$cand"
			break
		fi
	done
fi
INPUT="${1:-$ROOT/tests/decompilebench/corpus/binary_search-gcc-O0}"
OUT="${2:-/tmp/retdec-neural-out.c}"

if [[ ! -x "${DEC}" ]]; then
	echo "decompiler not found: ${DEC}" >&2
	echo "build with: bash scripts/wsl_build_neural.sh" >&2
	exit 1
fi
if [[ -z "${MODEL}" || ! -f "${MODEL}" ]]; then
	echo "model not found; stage with: bash scripts/fetch_qwen_gguf.sh" >&2
	exit 1
fi

export RETDEC_NEURAL_REFINE=1
export RETDEC_NEURAL_MODEL="${MODEL}"
export RETDEC_NEURAL_MODEL_SHA256="${RETDEC_NEURAL_MODEL_SHA256:-03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8}"
export RETDEC_NEURAL_TIER_MAX="${RETDEC_NEURAL_TIER_MAX:-1}"
export RETDEC_NEURAL_N_GPU_LAYERS="${RETDEC_NEURAL_N_GPU_LAYERS:--1}"
export RETDEC_PROFILE_JSON=auto

echo "decompiler=${DEC}"
echo "model=${MODEL}"
echo "input=${INPUT}"
"${DEC}" "${INPUT}" -o "${OUT}"
echo "wrote ${OUT}"
if [[ -f "${OUT}.refined.c" ]]; then
	echo "refined ${OUT}.refined.c"
	wc -l "${OUT}" "${OUT}.refined.c"
elif [[ -f "${OUT}.refinement-manifest.json" ]]; then
	echo "no refined sidecar; manifest:"
	cat "${OUT}.refinement-manifest.json"
else
	echo "no sidecar (refine off, load failed, or llama.cpp not linked)"
fi
