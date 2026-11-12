#!/usr/bin/env bash
# Configure WSL build/linux with neural refinement and build the decompiler.
#
# The inference backend is Forge (third_party/forge, src/neural/forge_inference.cpp):
# vendored, nothing is fetched. RETDEC_NEURAL_GPU_OFFLOAD=ON only makes the GPU
# Forge's default device (GPU runtimes are loaded at run time; no CUDA toolkit
# is needed to build, and an unavailable device falls back to the CPU).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

cmake --preset full-linux-debug \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_ENABLE_NEURAL=ON \
	-DRETDEC_NEURAL_GPU_OFFLOAD=ON

JOBS="$(nproc 2>/dev/null || echo 4)"
cmake --build build/linux --target retdec-neural retdec-decompiler --parallel "${JOBS}"
echo "Built retdec-decompiler with Forge neural refinement (RETDEC_NEURAL_GPU_OFFLOAD=ON)"
echo "Run: RETDEC_NEURAL_REFINE=1 RETDEC_NEURAL_MODEL=<model.fpk|model.gguf|hf-dir> retdec-decompiler ..."
