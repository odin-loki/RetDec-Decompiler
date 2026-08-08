# Neural refinement

Architecture for offline, verified neural post-processing (MASTER-UPGRADE-PLAN Phase 4).

## Status

- `retdec::neural` library with mock inference backend (tests pass without GGUF).
- llama.cpp backend: planned (`RETDEC_ENABLE_NEURAL`, pinned in `cmake/deps.cmake`).
- Integration point: after `decompileToLlvmIr` C emission in `src/retdec/retdec.cpp`.

## Artefacts

| File | Role |
|------|------|
| `output.c` | Deterministic decompiler output (untouched) |
| `output.refined.c` | Neural refinement (gated) |
| `refinement-manifest.json` | Per-edit gate results and model identity |

## Gates

1. **Compile** — refined C must compile.
2. **Structural** — CFG edit distance within threshold.
3. **Differential** — observable behaviour matches on test inputs.

On any gate failure, deterministic output is returned silently.

## Offline guarantee

Build with `RETDEC_NEURAL_OFFLINE_ONLY=ON` (planned) and run with `--no-network`.
Inference uses in-process llama.cpp only; no cloud API.

## Model provenance

Pin GGUF SHA-256 in config; verify at load. Document quantisation and licence
(Qwen3.5 — confirm Apache-2.0 for your release).
