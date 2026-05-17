#!/usr/bin/env bash
# Cross-compile RetDec for Windows (MinGW-w64) from WSL into build/linux/mingw-w64-release/.
# Requires a native Linux tblgen (build full-linux-debug first, or set RETDEC_LLVM_TABLEGEN).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/retdec-env.sh
source "${SCRIPT_DIR}/lib/retdec-env.sh"

NATIVE_TBLGEN="${RETDEC_LLVM_TABLEGEN:-${RETDEC_BUILD_DEBUG}/deps/install/llvm/bin/llvm-tblgen}"
if [[ ! -x "${NATIVE_TBLGEN}" ]]; then
	echo "ERROR: llvm-tblgen not found at: ${NATIVE_TBLGEN}" >&2
	echo "  Build native first: bash scripts/wsl_configure_nosudo.sh && cmake --build \"${RETDEC_BUILD_DEBUG}\" -j\"\$(nproc)\"" >&2
	echo "  Or set RETDEC_LLVM_TABLEGEN to your llvm-tblgen path." >&2
	exit 1
fi

mkdir -p "${RETDEC_BUILD_MINGW}" "${RETDEC_INSTALL_MINGW}"
echo "[$(date +%H:%M:%S)] Configuring MinGW cross -> ${RETDEC_BUILD_MINGW}"

cmake \
	-S "${RETDEC_ROOT}" \
	-B "${RETDEC_BUILD_MINGW}" \
	-G Ninja \
	-DCMAKE_TOOLCHAIN_FILE="${RETDEC_ROOT}/cmake/toolchains/windows-mingw-w64.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="${RETDEC_INSTALL_MINGW}" \
	-DRETDEC_TESTS=OFF \
	-DRETDEC_ENABLE_GOOGLETEST=OFF \
	-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
	-DRETDEC_LLVM_TABLEGEN="${NATIVE_TBLGEN}" \
	-DRETDEC_ENABLE_RETDEC_DECOMPILER=ON \
	-DRETDEC_ENABLE_RETDEC=ON \
	-DRETDEC_ENABLE_FILEFORMAT=ON \
	-DRETDEC_ENABLE_BIN2LLVMIR=ON \
	-DRETDEC_ENABLE_LLVMIR2HLL=ON \
	-DRETDEC_ENABLE_DEMANGLER=ON \
	-DRETDEC_ENABLE_COMMON=ON \
	-DRETDEC_ENABLE_CONFIG=ON \
	-DRETDEC_ENABLE_AR_EXTRACTOR=ON \
	-DRETDEC_ENABLE_MACHO_EXTRACTOR=ON \
	-DRETDEC_ENABLE_UNPACKER=ON \
	-DRETDEC_ENABLE_UNPACKERTOOL=ON \
	-DRETDEC_ENABLE_CAPSTONE2LLVMIR=ON \
	2>&1 | tee "${RETDEC_BUILD_MINGW}/cmake-configure.log"

echo "[$(date +%H:%M:%S)] Configure exit: ${PIPESTATUS[0]}"
exit "${PIPESTATUS[0]}"
