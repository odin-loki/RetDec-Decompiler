#!/usr/bin/env bash
# Stage the default text-only 9B Instruct GGUF (Qwen 3.5; Qwen 3.6 has no 9B).
# Delegates to scripts/fetch_qwen_gguf.sh (Unsloth llama.cpp-native GGUF).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec bash "${ROOT}/scripts/fetch_qwen_gguf.sh"
