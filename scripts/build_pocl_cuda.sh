#!/usr/bin/env bash
# build_pocl_cuda.sh
# Builds POCL from source with its CUDA backend so the RTX 3090 appears
# as a genuine OpenCL device in WSL2 (where NVIDIA's own OpenCL ICD is absent).
#
# Run this in one terminal.
# Run download_model.sh in a second terminal at the same time.
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -eo pipefail

log()  { echo ""; echo ">>> $*"; }
ok()   { echo "    [ok]   $*"; }
skip() { echo "    [skip] $*"; }

# ── 1. Fix the broken ICD file ────────────────────────────────────────────────
log "Fixing NVIDIA ICD (was pointing at non-existent WSL path)..."
echo "libnvidia-opencl.so.1" | sudo tee /etc/OpenCL/vendors/nvidia.icd
ok "nvidia.icd reset"

# ── 2. Install build dependencies ─────────────────────────────────────────────
log "Installing build dependencies..."
sudo apt-get install -y \
    cmake pkg-config libhwloc-dev zlib1g-dev \
    ocl-icd-dev ocl-icd-libopencl1 \
    llvm-18-dev libclang-18-dev clang-18 \
    libclang-cpp18-dev \
    git
ok "Dependencies installed"

# ── 3. Clone POCL ─────────────────────────────────────────────────────────────
POCL_SRC="$HOME/pocl-src"

if [[ -d "$POCL_SRC/.git" ]]; then
    log "Updating existing POCL clone..."
    git -C "$POCL_SRC" pull --ff-only
else
    log "Cloning POCL..."
    git clone --depth 1 https://github.com/pocl/pocl "$POCL_SRC"
fi
ok "POCL source ready at $POCL_SRC"

# ── 4. Configure ──────────────────────────────────────────────────────────────
log "Configuring POCL with CUDA backend..."
cmake -S "$POCL_SRC" -B "$POCL_SRC/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_CUDA=ON \
    -DWITH_LLVM_CONFIG=/usr/bin/llvm-config-18 \
    -DENABLE_ICD=ON \
    -DCMAKE_INSTALL_PREFIX=/usr/local 2>&1
ok "Configuration done"

# ── 5. Build ──────────────────────────────────────────────────────────────────
CORES="$(nproc)"
log "Building POCL using $CORES cores (takes ~15 min)..."
cmake --build "$POCL_SRC/build" -j"$CORES"
ok "Build complete"

# ── 6. Install ────────────────────────────────────────────────────────────────
log "Installing POCL..."
sudo cmake --install "$POCL_SRC/build"
sudo ldconfig
ok "POCL installed"

# ── 7. Verify ─────────────────────────────────────────────────────────────────
log "Verifying OpenCL devices..."
clinfo --list

echo ""
echo "============================================================"
echo " If the RTX 3090 appears above, OpenCL is working."
echo " Build RetDec with:"
echo "   cmake --preset core-release -DRETDEC_ENABLE_OPENCL=ON"
echo "   cmake --build build/core-release --parallel"
echo "============================================================"
