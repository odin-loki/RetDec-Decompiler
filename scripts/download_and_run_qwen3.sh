#!/usr/bin/env bash
# download_and_run_qwen3.sh
# Downloads Qwen3-Coder-30B-A3B Q4_K_M and builds llama.cpp with CUDA.
# RTX 3090 has 24 GB VRAM — the full Q4_K_M (~18 GB) fits with room to spare.
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -eo pipefail

MODEL_DIR="$HOME/.retdec/models"
MODEL_FILE="Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf"
MODEL_PATH="$MODEL_DIR/$MODEL_FILE"
HF_URL="https://huggingface.co/bartowski/Qwen3-Coder-30B-A3B-Instruct-GGUF/resolve/main/${MODEL_FILE}?download=true"
LLAMA_DIR="$HOME/llama.cpp"

echo "============================================================"
echo " Qwen3-Coder-30B-A3B  —  CUDA setup for RTX 3090"
echo "============================================================"

# ── 1. Install build deps ─────────────────────────────────────────────────────
echo ""
echo ">>> Installing build dependencies..."
sudo apt-get install -y \
    git cmake ninja-build \
    cuda-toolkit-12-9 \
    libcurl4-openssl-dev 2>&1 | grep -E "^(Get|Inst|Setting|Err|E:)" || true

# ── 2. Download the model (resumable) ─────────────────────────────────────────
mkdir -p "$MODEL_DIR"

if [[ -f "$MODEL_PATH" && "$(stat -c%s "$MODEL_PATH" 2>/dev/null)" -gt $((15 * 1024 * 1024 * 1024)) ]]; then
    echo ""
    echo "    [skip] Model already downloaded: $MODEL_PATH"
else
    echo ""
    echo ">>> Downloading Qwen3-Coder-30B-A3B-Instruct Q4_K_M (~18 GB)..."
    echo "    Destination: $MODEL_PATH"
    echo "    Download is resumable — re-run this script if interrupted."
    echo ""
    wget --progress=bar:force --continue \
        "$HF_URL" \
        -O "$MODEL_PATH"
    echo ""
    echo "    [ok] Downloaded: $(du -h "$MODEL_PATH" | cut -f1)"
fi

# ── 3. Build llama.cpp with CUDA ──────────────────────────────────────────────
if [[ -x "$LLAMA_DIR/build/bin/llama-server" ]]; then
    echo ""
    echo "    [skip] llama.cpp already built at $LLAMA_DIR"
else
    echo ""
    echo ">>> Cloning llama.cpp..."
    git clone --depth 1 https://github.com/ggerganov/llama.cpp "$LLAMA_DIR" 2>&1 || \
        git -C "$LLAMA_DIR" pull

    echo ""
    echo ">>> Building llama.cpp with CUDA (this takes 3-5 minutes)..."
    export PATH="/usr/local/cuda/bin:$PATH"
    cmake -S "$LLAMA_DIR" -B "$LLAMA_DIR/build" \
        -G Ninja \
        -DGGML_CUDA=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLAMA_CURL=ON
    cmake --build "$LLAMA_DIR/build" --config Release -j"$(nproc)"
    echo "    [ok] llama.cpp built with CUDA"
fi

# ── 4. Write a startup script ─────────────────────────────────────────────────
RUNNER="$HOME/.retdec/run_qwen3_server.sh"
cat > "$RUNNER" << SCRIPT
#!/usr/bin/env bash
# Starts the Qwen3 inference server on http://localhost:8080
# OpenAI-compatible API — RetDec can query it for decompilation hints.
export PATH="/usr/local/cuda/bin:\$PATH"
export LD_LIBRARY_PATH="/usr/local/cuda/lib64:\$LD_LIBRARY_PATH"

exec "${LLAMA_DIR}/build/bin/llama-server" \\
    --model "${MODEL_PATH}" \\
    --host 0.0.0.0 \\
    --port 8080 \\
    --n-gpu-layers 99 \\
    --ctx-size 32768 \\
    --threads "\$(nproc)" \\
    --flash-attn \\
    --verbose
SCRIPT
chmod +x "$RUNNER"

# ── 5. Summary ────────────────────────────────────────────────────────────────
echo ""
echo "============================================================"
echo " Done"
echo "============================================================"
echo ""
echo " Model:   $MODEL_PATH"
echo " Server:  $LLAMA_DIR/build/bin/llama-server"
echo " Startup: $RUNNER"
echo ""
echo " To start the inference server on the RTX 3090:"
echo "   bash $RUNNER"
echo ""
echo " The server exposes an OpenAI-compatible API at:"
echo "   http://localhost:8080/v1/chat/completions"
echo ""
echo " Test it once running:"
echo "   curl http://localhost:8080/v1/models"
echo ""
