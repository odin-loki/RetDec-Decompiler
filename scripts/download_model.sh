#!/usr/bin/env bash
# download_model.sh
# Downloads Qwen3-Coder-30B-A3B-Instruct Q4_K_M (~18 GB) from HuggingFace.
# Fully resumable — re-run if interrupted.
#
# @copyright (c) 2024 Odin Loch Trading as Imortek

set -eo pipefail

MODEL_DIR="$HOME/.retdec/models"
MODEL_FILE="Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf"
MODEL_PATH="$MODEL_DIR/$MODEL_FILE"
URL="https://huggingface.co/bartowski/Qwen3-Coder-30B-A3B-Instruct-GGUF/resolve/main/${MODEL_FILE}?download=true"

mkdir -p "$MODEL_DIR"

if [[ -f "$MODEL_PATH" && "$(stat -c%s "$MODEL_PATH" 2>/dev/null)" -gt $((15 * 1024 * 1024 * 1024)) ]]; then
    echo "[skip] Already downloaded: $MODEL_PATH ($(du -h "$MODEL_PATH" | cut -f1))"
    exit 0
fi

echo "Downloading $MODEL_FILE (~18 GB) — resumable, re-run if interrupted."
echo "Destination: $MODEL_PATH"
echo ""

wget --progress=bar:force --continue "$URL" -O "$MODEL_PATH"

echo ""
echo "[ok] Downloaded: $(du -h "$MODEL_PATH" | cut -f1)"
echo "     Path: $MODEL_PATH"
