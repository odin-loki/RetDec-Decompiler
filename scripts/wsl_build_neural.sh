#!/usr/bin/env bash
# Configure WSL build/linux with llama.cpp (b10451) and build the decompiler.
# Creates the ExternalProject include dir so CMake can import the llama target
# before the first fetch/install.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

INC="${ROOT}/build/linux/deps/install/llamacpp/include"
LIB="${ROOT}/build/linux/deps/install/llamacpp/lib"
mkdir -p "${INC}" "${LIB}"

cmake -S . -B build/linux \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_ENABLE_NEURAL=ON \
	-DRETDEC_ENABLE_LLAMACPP=ON \
	-DLLAMACPP_URL="https://github.com/ggml-org/llama.cpp/archive/refs/tags/b10451.zip" \
	-DLLAMACPP_ARCHIVE_SHA256="b04aeb511cc05451a410437eacd5a2d64a3130c27f10a54a23ad948369816cad"

JOBS="$(nproc 2>/dev/null || echo 4)"
cmake --build build/linux --target llamacpp-project retdec-neural retdec-decompiler --parallel "${JOBS}"
echo "Built llama.cpp + retdec-decompiler (RETDEC_ENABLE_LLAMACPP=ON)"
