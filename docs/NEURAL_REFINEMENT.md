# Neural refinement

Imortek **2.0.24**. Offline, gated post-processing **after** deterministic
C emission. Not a decompiler pass and not GPU acceleration of lifting.

## Status

- `retdec::neural` runs on **Forge** (`third_party/forge`, vendored; nothing
  is fetched): `createForgeInference()` in `src/neural/forge_inference.cpp`
  uses Forge's C++ inference API (`forge::inference::model` / `session`, its
  sampler chain and its GBNF `grammar_constraint`). llama.cpp is no longer
  used; its build options, its `deps/` ExternalProject and its pin are gone
  (see CHANGELOG Unreleased). Release installers build with
  `RETDEC_ENABLE_NEURAL=ON` (GPU default **OFF**). CTest jobs keep neural
  **OFF** and do not download the GGUF; `retdec-neural-forge-tests` runs the
  backend end to end on Forge's tiny random-weight model
  (`tests/neural/data/tiny_f32.fpk`).
- Runtime refine is **on by default** when the shipped GGUF is present.
  - Build: `RETDEC_ENABLE_NEURAL` (library on Forge; the decompiler links
    an empty stub when it is OFF).
  - Run: looks for `share/retdec/models/Qwen3.5-9B-Q4_K_M.gguf` (or
    `RETDEC_NEURAL_MODEL`). Disable with `RETDEC_NEURAL_REFINE=0`.
  - Model formats: a Forge `.fpk` loads as is; a `.gguf` or a Hugging Face
    directory is imported once into Forge's model cache (`$FORGE_CACHE`,
    default `~/.cache/forge`) under the same content-addressed name `forge
    run` uses, so the two share imports. `RETDEC_NEURAL_IMPORT_CODEC` picks
    the import codec (default `f32`). An installed RetDec ships Forge's
    import templates in `share/retdec/support/forge/templates`.
- **Shipped GGUF:** Unsloth `Qwen3.5-9B-Q4_K_M.gguf` (SHA-256
  `03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8`) is
  pinned in `support/models.json`. GitHub Release assets cap at 2 GB, so
  job `neural-gguf` (after the OS installer jobs) uploads 1900 MiB
  `*.gguf.partaa` / `.partab` / … pieces onto the **same Release**.
  `install.sh` joins them into `share/retdec/models/` when the parts sit
  next to the package. Or: `scripts/join_qwen_gguf.sh`.
- **Compile gate is implemented:** `cc`/`gcc -fsyntax-only` (`src/neural/gates.cpp`).
  Skip with `RETDEC_NEURAL_SKIP_COMPILE_GATE=1`.
- **Differential gate is not implemented:** `RETDEC_NEURAL_DIFF_GATE=1`
  prints a warning and **skips** (treated as pass). Decompiled C is never
  compiled-and-executed as a test oracle.
- **9B Instruct path:** Qwen 3.6 has no 9B (27B / 35B only). The 9B
  checkpoint is **Qwen 3.5**. Stage the GGUF with
  `bash scripts/fetch_qwen_gguf.sh` (Unsloth `Qwen3.5-9B-Q4_K_M.gguf`, the
  allowlisted file); Forge imports it on first load. Do not load `mmproj`.
- Multimodal `mmproj` / `-VL-` filenames are rejected.
- Prompts use the Qwen Instruct chat template. Thinking is off unless
  `RETDEC_NEURAL_THINKING=1`.
- Sampler: Naming / Comments / StructFields default to temperature 0.
  Other tiers use temperature 0.6, top-p 0.95, top-k 20 (Qwen Instruct).
- Prompt-prefix KV reuse is **off** unless `RETDEC_NEURAL_REUSE_KV=1`.
  When on, the manifest records `"reuse_kv":true` (non-reproducible).
- **N15 Naming GBNF:** the Naming tier sets a Forge `grammar_constraint`
  (`namingRenameMapGbnf`) so the model emits a JSON rename map, then
  `applyJsonRenameMap` rewrites identifiers in the deterministic C.
  Other tiers stay unconstrained. `retdec-neural-forge-tests` checks that
  grammar-constrained output parses as JSON on the tiny model.
- **Device:** Forge loads GPU runtimes at run time. `RETDEC_NEURAL_DEVICE`
  (or Forge's own `FORGE_DEVICE`) selects one: `cpu`, `cuda[:N]`, `vulkan`,
  `hip`, `metal`, ... `RETDEC_NEURAL_N_GPU_LAYERS` (still written by the GUI
  AI Assistant from its GPU toggle and Settings → ML device: CPU → `0`, GPU
  or Auto → `-1`) maps `0` to the CPU and anything else to the GPU (Metal on
  Apple, CUDA elsewhere); Forge offloads the whole graph or nothing, so the
  count has no finer meaning. An unavailable device falls back to the CPU
  with a warning. `RETDEC_NEURAL_THREADS` sets `FORGE_THREADS`.
  The llama.cpp-era MTP switch no longer applies.

When `RETDEC_ENABLE_NEURAL` is OFF, `src/retdec/neural_refine_stub.cpp`
provides an empty `maybeRefineDecompilerOutput`.

**Build-time:** `RETDEC_NEURAL_GPU_OFFLOAD=ON` only makes the GPU Forge's
default device; it needs no CUDA toolkit (runtimes are loaded at run time).
CI default is OFF.

**Runtime (decompiler / GUI child `QProcess` env):**

- `RETDEC_NEURAL_REFINE` (default on; `0` / `false` / `off` disables)
- `RETDEC_NEURAL_MODEL` (default: `share/retdec/models/Qwen3.5-9B-Q4_K_M.gguf`)
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
`retdec-decompiler` child as these env vars when the run is interactive.
`retdec-gui --headless-decompile` sets `quitWhenDecompileFinishes`, which
calls `buildDecompilerProcessEnvironment(..., applyInteractiveOverrides=false)`
and therefore does **not** inject `RETDEC_NEURAL_*` from saved ML settings.

Refinement latency is `neural_refine_wall_s` in DecompileBench; it is
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

1. **Compile** — `gcc`/`cc -fsyntax-only` (skip with `RETDEC_NEURAL_SKIP_COMPILE_GATE=1`).
2. **Structural** — non-empty; size sanity vs original; tree-sitter C when built.
3. **Differential** — **not implemented.** `RETDEC_NEURAL_DIFF_GATE=1` warns and skips; C is never executed.

On compile or structural failure, deterministic output is kept.

## Offline

Build with `RETDEC_NEURAL_OFFLINE_ONLY=ON` and run with `RETDEC_NO_NETWORK=1`.
Inference is in-process Forge only.

Hook: `neural::maybeRefineDecompilerOutput` in
`include/retdec/neural/decompile_hook.h`, called from `src/retdec/retdec.cpp`.
