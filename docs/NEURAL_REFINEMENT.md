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
- Sampler: Naming / Comments / StructFields default to temperature 0.
  Other tiers use temperature 0.6, top-p 0.95, top-k 20 (Qwen Instruct).
- Prompt-prefix KV reuse is **off** unless `RETDEC_NEURAL_REUSE_KV=1`.
  When on, the manifest records `"reuse_kv":true` (non-reproducible).
- **N15 Naming GBNF:** the Naming tier sets `llama_sampler_init_grammar`
  (`namingRenameMapGbnf`) so the model emits a JSON rename map, then
  `applyJsonRenameMap` rewrites identifiers in the deterministic C.
  Other tiers stay unconstrained. Requires a real GGUF; mock tests cover
  the apply/GBNF string only.
- `RETDEC_NEURAL_GPU_OFFLOAD=ON` passes `GGML_CUDA` into llama.cpp.
  `RETDEC_NEURAL_N_GPU_LAYERS` sets `llama_model_params.n_gpu_layers`
  (`-1` = all layers, `0` = CPU). The GUI AI Assistant (Tools →
  AI Assistant…) writes this env from the GPU toggle and Settings → ML
  device (CPU → `0`; GPU or Auto → `-1`) before `loadModel`.
  `RETDEC_NEURAL_MTP=1` sets `llama_model_params.load_mtp`. Speculative
  MTP decode has no C API at b10451.

**Build-time:** `RETDEC_NEURAL_GPU_OFFLOAD=ON` compiles `GGML_CUDA` into
llama.cpp. CI default is OFF.

**Runtime (decompiler / GUI child):**
- `RETDEC_NEURAL_REFINE=1`
- `RETDEC_NEURAL_MODEL`
- `RETDEC_NEURAL_MODEL_SHA256`
- `RETDEC_NEURAL_CTX` (default 4096)
- `RETDEC_NEURAL_MAX_TOKENS`
- `RETDEC_NEURAL_THREADS`
- `RETDEC_NEURAL_N_BATCH`
- `RETDEC_NEURAL_N_GPU_LAYERS` (`-1` all, `0` CPU)
- `RETDEC_NEURAL_TEMPERATURE` / `RETDEC_NEURAL_TOP_P` / `RETDEC_NEURAL_TOP_K`
- `RETDEC_NEURAL_REUSE_KV` (default off)
- `RETDEC_NEURAL_TIER_MAX`
- `RETDEC_NEURAL_THINKING`
- `RETDEC_NEURAL_ALLOW_NETWORK`
- `RETDEC_NEURAL_SKIP_COMPILE_GATE`

GUI: Settings → ML model path that exists on disk is passed to the
decompiler child as these env vars. Headless `--headless-decompile` does
not apply saved ML settings.

- Compile gate is `cc`/`gcc -fsyntax-only`. Differential gate is **not
  implemented**: `RETDEC_NEURAL_DIFF_GATE=1` warns and skips.
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
export RETDEC_NEURAL_MODEL="$PWD/models/Qwen3.5-9B-Q4_K_M.gguf"
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
3. **Differential** — **not implemented.** `RETDEC_NEURAL_DIFF_GATE=1` warns and skips; C is never executed.

On any gate failure, deterministic output is kept.

## Offline

Build with `RETDEC_NEURAL_OFFLINE_ONLY=ON` and run with `RETDEC_NO_NETWORK=1`.
Inference is in-process llama.cpp only.
