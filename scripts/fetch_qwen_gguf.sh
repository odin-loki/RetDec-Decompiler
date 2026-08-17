#!/usr/bin/env bash
# Stage a llama.cpp-native Qwen 3.5 9B Q4_K_M GGUF under models/.
# Qwen 3.6 has no 9B (27B / 35B only). Ollama qwen3.5:9b blobs do not load
# on llama.cpp b10451 (rope.dimension_sections length 3; PR 25334 still open).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST_DIR="${RETDEC_MODEL_DIR:-$ROOT/models}"
DEST="$DEST_DIR/Qwen3.5-9B-Q4_K_M.gguf"
SHA="03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8"
HF_URL="https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/main/Qwen3.5-9B-Q4_K_M.gguf"

mkdir -p "$DEST_DIR"

print_exports() {
    echo "[ok] $1"
    echo "     export RETDEC_NEURAL_MODEL=$1"
    echo "     export RETDEC_NEURAL_REFINE=1"
    echo "     export RETDEC_NEURAL_MODEL_SHA256=$SHA"
}

already_ok() {
    [ -f "$1" ] && [ "$(stat -c%s "$1" 2>/dev/null || stat -f%z "$1")" -gt 1000000000 ]
}

for cand in "$DEST" "$DEST_DIR/Qwen3.5-9B-Q4_K_M.unsloth.gguf"; do
    if already_ok "$cand"; then
        print_exports "$cand"
        exit 0
    fi
done

if command -v ollama >/dev/null 2>&1; then
    echo "[note] ollama is on PATH; qwen3.5:9b GGUF blobs are not used"
    echo "       (incompatible with llama.cpp b10451). Downloading Unsloth GGUF."
fi

if command -v wget >/dev/null 2>&1; then
    wget --progress=bar:force --continue "$HF_URL" -O "$DEST"
elif command -v curl >/dev/null 2>&1; then
    curl -L --continue-at - -o "$DEST" "$HF_URL"
else
    echo "need wget or curl" >&2
    exit 1
fi

print_exports "$DEST"
