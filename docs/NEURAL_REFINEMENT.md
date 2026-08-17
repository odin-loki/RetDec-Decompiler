# Neural refinement

Offline, gated post-processing after the deterministic decompiler.

## Status

- `retdec::neural` with mock + optional llama.cpp (`RETDEC_ENABLE_LLAMACPP`).
  Pin: **b10451** (Qwen 3.5 architecture / MTP load flag). Default installers
  keep llama.cpp OFF.
- **9B Instruct path:** Qwen 3.6 has no 9B (27B / 35B only). The 9B checkpoint
  is **Qwen 3.5**. Stage a **llama.cpp-native** GGUF with
  `bash scripts/fetch_qwen_gguf.sh`. Prefer the Unsloth
  `Qwen3.5-9B-Q4_K_M.gguf` (b10451). `ollama pull qwen3.5:9b` writes
  `rope.dimension_sections` length 3; b10451 expects 4 (llama.cpp PR 25334
  is still open). Do not load `mmproj`.
- Multimodal `mmproj` / `-VL-` filenames are rejected.
- Prompts use the Qwen Instruct chat template. Thinking is off unless
  `RETDEC_NEURAL_THINKING=1`.
- Sampler: temperature 0.6, top-p 0.95, top-k 20 (Qwen Instruct defaults).
- Later tiers reuse the shared prompt-prefix KV (`reuseKvPrefix`).
- `RETDEC_NEURAL_GPU_OFFLOAD=ON` passes `GGML_CUDA` into llama.cpp.
  `RETDEC_NEURAL_MTP=1` sets `llama_model_params.load_mtp`. Speculative
  MTP decode has no C API at b10451.
- Compile gate + optional differential gate (`RETDEC_NEURAL_DIFF_GATE=1`).
- Refinement latency is `neural_refine_wall_s` in DecompileBench; it is
  not mixed into `mean_wall_s`.

## Build and run

```bash
bash scripts/fetch_qwen_gguf.sh
bash scripts/wsl_build_neural.sh
bash scripts/run_neural_refine.sh
```

Or by hand:

```bash
export RETDEC_NEURAL_REFINE=1
export RETDEC_NEURAL_MODEL="$PWD/models/Qwen3.5-9B-Instruct-Q4_K_M.gguf"
export RETDEC_NEURAL_MODEL_SHA256=03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8
./build/linux/src/retdec-decompiler/retdec-decompiler in.bin -o out.c
```

Deterministic `out.c` is unchanged. Accepted refine writes `out.refined.c`.

## Tiers

| Tier | Name | Env |
|------|------|-----|
| 1 | Naming | default |
| 2 | Comments | default |
| 3 | Struct fields | default |
| 4 | Idiom recovery | `RETDEC_NEURAL_TIER_MAX=4` |
| 5 | Full rewrite | `RETDEC_NEURAL_TIER_MAX=5` (human review required) |

First smoke: `RETDEC_NEURAL_TIER_MAX=1`.

## Gates

1. **Compile** — `gcc -fsyntax-only` (skip with `RETDEC_NEURAL_SKIP_COMPILE_GATE=1`).
2. **Structural** — non-empty; size sanity vs original.
3. **Differential** — `RETDEC_NEURAL_DIFF_GATE=1`.

On any gate failure, deterministic output is kept.

## Offline

Build with `RETDEC_NEURAL_OFFLINE_ONLY=ON` and run with `RETDEC_NO_NETWORK=1`.
Inference is in-process llama.cpp only.
