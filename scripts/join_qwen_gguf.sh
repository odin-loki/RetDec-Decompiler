#!/usr/bin/env bash
# Concatenate GitHub Release GGUF parts (split -b 1900M → .partaa .partab …)
# and SHA-256 the reconstructed file against the pinned Unsloth Q4_K_M hash.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHA="03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8"
PART_DIR="${1:-.}"
DEST_DIR="${RETDEC_MODEL_DIR:-$ROOT/models}"
DEST="$DEST_DIR/Qwen3.5-9B-Q4_K_M.gguf"
PREFIX="Qwen3.5-9B-Q4_K_M.gguf.part"

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo ""
    fi
}

shopt -s nullglob
PARTS=( "$PART_DIR"/"$PREFIX"* )
if [ "${#PARTS[@]}" -eq 0 ]; then
    echo "no parts matching $PART_DIR/$PREFIX*" >&2
    echo "download Qwen3.5-9B-Q4_K_M.gguf.partaa (and siblings) from the GitHub Release" >&2
    exit 1
fi

mkdir -p "$DEST_DIR"
rm -f "$DEST"
# glob order is lexicographic (partaa, partab, partac, …)
cat "${PARTS[@]}" > "$DEST"

got="$(sha256_of "$DEST")"
if [ -z "$got" ]; then
    echo "[warn] no sha256sum/shasum; skip verify for $DEST" >&2
elif [ "$got" != "$SHA" ]; then
    echo "[error] SHA-256 mismatch for $DEST" >&2
    echo "        expected $SHA" >&2
    echo "        got      $got" >&2
    exit 1
fi

echo "[ok] $DEST"
echo "     refine is on by default; RETDEC_NEURAL_REFINE=0 disables"
echo "     export RETDEC_NEURAL_MODEL=$DEST"
echo "     export RETDEC_NEURAL_MODEL_SHA256=$SHA"
