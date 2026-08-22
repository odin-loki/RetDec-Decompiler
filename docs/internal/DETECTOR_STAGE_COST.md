# Measuring detector-stage cost (C9)

Filename-derived stem augment is gone from the analysis pipeline. Remaining
detector cost is the real post-pipeline block in `src/retdec/retdec.cpp`
(container, algo, idiom, sort, concurrency, then semantic export).

This note describes how to **measure** that cost. It does **not** add a public
CLI flag.

## Existing instrumentation

| Mechanism | What it reports |
|-----------|-----------------|
| `RETDEC_PROFILE_JSON=auto` (or `1`) | Writes `<output>.profile.json`. The detector block is scoped as `analysis.detectors` via `Profiler::instance().measure("analysis.detectors")` in `retdec.cpp`. |
| `RETDEC_BIN2LLVMIR_DIAG=1` | Logs `post_pipeline_analysis_wall_ms` (detectors **plus** OpenCL host recovery and any enabled type-inference loop). |
| `RETDEC_INCREMENTAL_CACHE=0` | Disables the function-analysis cache so the first-run detector cost is not hidden by a sidecar hit. |
| `RETDEC_SKIP_SEMANTIC_RECOVERY` | Opt-in skip of detectors + `exportSemanticRecovery`. Set to any non-empty value except `0`. Default decompile still runs detectors. |

See [docs/PERFORMANCE.md](../PERFORMANCE.md) and the env table in [docs/README.md](../README.md).

## Opt-in skip: `RETDEC_SKIP_SEMANTIC_RECOVERY`

Env-only (no `--skip-detectors` CLI flag). Headless `--quit-when-done` does
**not** set this. Default F5 / default `retdec-decompiler` still run detectors.

When set and not `0`:

- Skips container / algo / idiom / sort / concurrency detectors and `exportSemanticRecovery`.
- Still calls `maybeRefineDecompilerOutput` (empty semantic context) so neural benches work.
- Does **not** skip OpenCL host recovery or other later stages.
- The `analysis.detectors` profile timer still wraps the skip (~0 ms).

## A/B procedure

```
RETDEC_PROFILE_JSON=1 ./retdec-decompiler sample -o /tmp/a.c
RETDEC_PROFILE_JSON=1 RETDEC_SKIP_SEMANTIC_RECOVERY=1 ./retdec-decompiler sample -o /tmp/b.c
```

Compare `analysis.detectors` total_ms.

Measured 2026-08-22 on `binary_search-gcc-O0` (Debug, WSL, warm):

| Run | wall_s | analysis.detectors ms | pipeline.pm_run ms |
|-----|--------|----------------------|--------------------|
| default | 1.305 | 55.65 | 589 |
| `RETDEC_SKIP_SEMANTIC_RECOVERY=1` | 1.210 | 3.30 | 584 |

Detectors are ~4% of a cold Debug run and ~50 ms when warm. They are not the 35× gap vs stock Release Docker (0.25 s).

Use a small gcc-O0 ELF from `tests/algorithm_recovery/corpus/` (or any ~14 KB
sample). Rebuild **only** `retdec` / `retdec-decompiler` after a local experiment — not LLVM.

Optional isolators for a cleaner first-run delta:

```bash
export RETDEC_INCREMENTAL_CACHE=0
export RETDEC_OCL_HOST=0
unset RETDEC_NEURAL_REFINE
```

Nearby knobs that are **not** substitutes:

| Knob | Actual effect |
|------|----------------|
| `RETDEC_OCL_HOST=0` | Skips OpenCL **host** recovery only (the block after detectors). |
| `RETDEC_TYPE_INFERENCE` | Extra type-inference loop; **off by default**. |
| `--disable-static-code-detection` | Disables YARA / statically-linked-code signatures (`setIsDetectStaticCode(false)`). Does **not** disable algo / sort / container / concurrency detectors. |
| `RETDEC_INCREMENTAL_CACHE` | Skips **unchanged** functions on re-runs; does not skip detectors on a cold run. |
| `RETDEC_NEURAL_REFINE` | Separate cost axis; leave unset when measuring detectors. |

On the 2026-08-17 six-ELF note in PERFORMANCE.md, post-pipeline analysis was
already only **12–24 ms** vs **550–750 ms** of LLVM / bin2llvmir. Re-measure
after the stem-augment deletion; do not reuse withdrawn algorithm-recovery F1
figures as a cost proxy.
