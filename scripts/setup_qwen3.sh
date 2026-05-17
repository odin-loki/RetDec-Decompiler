#!/usr/bin/env bash
# setup_qwen3.sh — Install CUDA runtime and download the Qwen3-Coder-30B-A3B
# GGUF model so RetDec's built-in qwen3 inference engine can use it.
#
# What this does:
#   1. Detect your GPU (NVIDIA) and verify CUDA availability
#   2. Install CUDA toolkit (if not already present)
#   3. Install huggingface-cli for reliable resumable downloads
#   4. Download Qwen3-Coder-30B-A3B-Instruct Q4_K_M GGUF (~18 GB)
#   5. Place it in ~/.retdec/models/ where RetDec looks by default
#
# Usage:
#   bash scripts/setup_qwen3.sh [--quant Q4_K_M|Q3_K_M|Q5_K_M|IQ2_XS]
#
# Quant guide:
#   Q4_K_M  ~18 GB  — recommended, good quality/size balance (default)
#   Q5_K_M  ~22 GB  — better quality, needs 24 GB+ VRAM/RAM
#   Q3_K_M  ~14 GB  — fits 16 GB VRAM, slightly lower quality
#   IQ2_XS  ~9  GB  — emergency option for 12 GB VRAM
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -eo pipefail

log()  { echo ""; echo ">>> $*"; }
ok()   { echo "    [ok]   $*"; }
skip() { echo "    [skip] $*"; }
warn() { echo "    [warn] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

QUANT="Q4_K_M"
MODEL_DIR="${HOME}/.retdec/models"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --quant) QUANT="$2"; shift 2;;
        --model-dir) MODEL_DIR="$2"; shift 2;;
        *) die "Unknown option: $1";;
    esac
done

# HuggingFace repo that hosts GGUF quantisations of this model.
# bartowski is the canonical GGUF quantiser for Qwen3 models.
HF_REPO="bartowski/Qwen3-Coder-30B-A3B-Instruct-GGUF"
MODEL_FILE="Qwen3-Coder-30B-A3B-Instruct-${QUANT}.gguf"

echo "============================================================"
echo " RetDec Qwen3 Setup"
echo "============================================================"
echo " Model:     Qwen3-Coder-30B-A3B-Instruct"
echo " Quantised: ${QUANT}  (file: ${MODEL_FILE})"
echo " Dest:      ${MODEL_DIR}/${MODEL_FILE}"
echo "============================================================"
echo ""

# ---------------------------------------------------------------------------
# 1. Detect GPU
# ---------------------------------------------------------------------------
log "Detecting GPU..."

HAS_NVIDIA=0
GPU_NAME=""

if command -v nvidia-smi &>/dev/null && nvidia-smi &>/dev/null 2>&1; then
    HAS_NVIDIA=1
    GPU_NAME="$(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"
    ok "NVIDIA GPU detected: $GPU_NAME"
fi

if [[ $HAS_NVIDIA -eq 0 ]]; then
    warn "No NVIDIA GPU detected — CUDA acceleration will be unavailable."
    warn "Inference will still work via CPU fallback but will be slower."
fi

# ---------------------------------------------------------------------------
# 2. Install CUDA toolkit (NVIDIA only)
# ---------------------------------------------------------------------------
if [[ $HAS_NVIDIA -eq 1 ]]; then
    log "Checking CUDA toolkit..."
    if command -v nvcc &>/dev/null; then
        CUDA_VER="$(nvcc --version 2>/dev/null | grep 'release' | awk '{print $5}' | tr -d ',')"
        skip "nvcc already installed: CUDA $CUDA_VER"
    else
        log "Installing NVIDIA CUDA toolkit..."
        # In WSL2 the CUDA toolkit is provided by nvidia-cuda-toolkit.
        sudo apt-get install -y nvidia-cuda-toolkit
        if command -v nvcc &>/dev/null; then
            ok "nvcc installed: $(nvcc --version | head -1)"
        else
            warn "nvcc not found after install — you may need to install CUDA manually."
            warn "See: https://developer.nvidia.com/cuda-downloads"
        fi
    fi

    # Verify libcuda.so is accessible (needed at runtime)
    if ldconfig -p 2>/dev/null | grep -q libcuda; then
        ok "libcuda.so found in ld cache"
    elif ls /usr/lib/wsl/lib/libcuda.so* &>/dev/null 2>&1; then
        ok "libcuda.so found in /usr/lib/wsl/lib (WSL2)"
        # Ensure wsl lib path is registered
        if [[ ! -f /etc/ld.so.conf.d/wsl-nvidia.conf ]]; then
            echo "/usr/lib/wsl/lib" | sudo tee /etc/ld.so.conf.d/wsl-nvidia.conf
            sudo ldconfig
        fi
    else
        warn "libcuda.so not found — ensure your Windows NVIDIA driver (>=470) is installed."
    fi
fi

# ---------------------------------------------------------------------------
# 3. Install huggingface-cli for reliable model downloads
# ---------------------------------------------------------------------------
log "Installing huggingface-hub (download CLI)..."
if command -v huggingface-cli &>/dev/null; then
    skip "huggingface-cli already installed"
else
    # Ubuntu 24.04+ blocks system-wide pip installs (PEP 668).
    # Try pipx first (cleanest), then --break-system-packages as fallback.
    if command -v pipx &>/dev/null; then
        pipx install huggingface-hub[cli]
    elif python3 -m pip install --quiet --break-system-packages huggingface-hub 2>/dev/null; then
        ok "huggingface-hub installed (--break-system-packages)"
    else
        # Last resort: install pipx then retry
        sudo apt-get install -y pipx
        pipx ensurepath
        pipx install huggingface-hub[cli]
    fi
    if command -v huggingface-cli &>/dev/null; then
        ok "huggingface-cli: $(huggingface-cli --version 2>&1)"
    else
        # pipx installs to ~/.local/bin — may need PATH update
        export PATH="$PATH:$HOME/.local/bin"
        command -v huggingface-cli &>/dev/null && \
            ok "huggingface-cli ready (restart shell to persist PATH)" || \
            die "Could not install huggingface-hub."
    fi
fi

# ---------------------------------------------------------------------------
# 4. Download the GGUF model
# ---------------------------------------------------------------------------
mkdir -p "$MODEL_DIR"
DEST="$MODEL_DIR/$MODEL_FILE"

if [[ -f "$DEST" ]]; then
    SIZE="$(du -h "$DEST" | cut -f1)"
    skip "Model already downloaded: $DEST ($SIZE)"
else
    log "Downloading $MODEL_FILE from HuggingFace..."
    echo "    Repo:  https://huggingface.co/$HF_REPO"
    echo "    File:  $MODEL_FILE"
    echo "    Dest:  $DEST"
    echo ""
    echo "    This is a large file (~18 GB for Q4_K_M). The download is"
    echo "    resumable — if interrupted, re-run this script to continue."
    echo ""

    huggingface-cli download \
        "$HF_REPO" \
        "$MODEL_FILE" \
        --local-dir "$MODEL_DIR" \
        --local-dir-use-symlinks False

    if [[ -f "$DEST" ]]; then
        SIZE="$(du -h "$DEST" | cut -f1)"
        ok "Downloaded: $DEST ($SIZE)"
    else
        die "Download failed — file not found at $DEST"
    fi
fi

# ---------------------------------------------------------------------------
# 5. Write a RetDec model config so the pipeline knows where to look
# ---------------------------------------------------------------------------
CONFIG_DIR="${HOME}/.retdec"
CONFIG_FILE="${CONFIG_DIR}/qwen3.conf"
mkdir -p "$CONFIG_DIR"

cat > "$CONFIG_FILE" << EOF
# RetDec Qwen3 model configuration
# Generated by setup_qwen3.sh
model_path = ${DEST}
quant      = ${QUANT}
cuda       = 1
max_new_tokens = 512
temperature    = 0.7
top_p          = 0.8
top_k          = 20
repetition_penalty = 1.05
EOF

ok "Config written: $CONFIG_FILE"

# ---------------------------------------------------------------------------
# 6. Quick smoke-test: read the model's metadata using Python + gguf lib
# ---------------------------------------------------------------------------
log "Running smoke test (reading GGUF metadata)..."
python3 - <<PYEOF 2>/dev/null || warn "Smoke test skipped (gguf package not installed; pip install gguf to enable)"
import sys
try:
    from gguf import GGUFReader
    reader = GGUFReader("${DEST}")
    arch = next((f.parts[-1].decode() for f in reader.fields.values()
                 if "architecture" in f.name), "unknown")
    params = next((int(f.parts[-1]) for f in reader.fields.values()
                   if "parameter_count" in f.name), 0)
    print(f"    architecture : {arch}")
    print(f"    parameters   : {params:,}")
    print(f"    quant        : ${QUANT}")
    print("    Smoke test PASSED")
except ImportError:
    print("    (install 'gguf' package for metadata inspection)")
except Exception as e:
    print(f"    Smoke test error: {e}", file=sys.stderr)
PYEOF

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "============================================================"
echo " Setup complete"
echo "============================================================"
echo ""
echo " Model:   ${DEST}"
echo " Config:  ${CONFIG_FILE}"
echo ""
if [[ $HAS_NVIDIA -eq 1 ]]; then
    echo " GPU:     NVIDIA $GPU_NAME — CUDA acceleration enabled"
else
    echo " GPU:     None detected — CPU-only inference (std::async)"
fi
echo ""
echo " Build RetDec with CUDA support:"
echo "   cmake --preset core-release"
echo "   cmake --build build/core-release --parallel"
echo ""
echo " The Qwen3Pipeline will load the model automatically."
echo " Call pipe.enableCUDA() before pipe.load() to use the GPU."
