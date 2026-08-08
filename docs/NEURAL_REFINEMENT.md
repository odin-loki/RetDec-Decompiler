# Neural refinement

Architecture for offline, verified neural post-processing (MASTER-UPGRADE-PLAN Phase 4).

## Status (v1.2.0)

- `retdec::neural` library with mock + optional llama.cpp backend (`RETDEC_ENABLE_LLAMACPP`).
- Decompile hook feeds **semantic detection JSON** from `config.functions` into prompts (step 8.4).
- Model SHA verified at load when `RETDEC_NEURAL_MODEL_SHA256` is set (step 8.8).
- Tiers 1–5 via `RETDEC_NEURAL_TIER_MAX` (default 3).
- Compile gate + optional differential gate (`RETDEC_NEURAL_DIFF_GATE=1`).

## Artefacts

| File | Role |
|------|------|
| `output.c` | Deterministic decompiler output (untouched) |
| `output.refined.c` | Neural refinement (gated) |
| `refinement-manifest.json` | Per-edit gate results and model identity |

## Tiers

| Tier | Name | Env |
|------|------|-----|
| 1 | Naming | default |
| 2 | Comments | default |
| 3 | Struct fields | default |
| 4 | Idiom recovery | `RETDEC_NEURAL_TIER_MAX=4` |
| 5 | Full rewrite | `RETDEC_NEURAL_TIER_MAX=5` (human review required) |

## Gates

1. **Compile** — `gcc -fsyntax-only` on refined C (skip with `RETDEC_NEURAL_SKIP_COMPILE_GATE=1`).
2. **Structural** — non-empty output; size sanity vs original.
3. **Differential** — stub until Triton/D-Helix (step 20).

On any gate failure, deterministic output is returned silently.

## Offline guarantee

Build with `RETDEC_NEURAL_OFFLINE_ONLY=ON` and run with `RETDEC_NO_NETWORK=1`.
Inference uses in-process llama.cpp only; no cloud API.

## Model provenance

Set `RETDEC_NEURAL_MODEL_SHA256` and verify at load via `scripts/doctor.sh`.
