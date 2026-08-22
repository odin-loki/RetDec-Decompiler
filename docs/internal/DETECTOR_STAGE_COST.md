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

See [docs/PERFORMANCE.md](../PERFORMANCE.md) and the env table in [docs/README.md](../README.md).

## No env to skip semantic detectors

There is **no** environment variable that skips the semantic-recovery / detector
block. Existing nearby knobs are not substitutes:

| Knob | Actual effect |
|------|----------------|
| `RETDEC_OCL_HOST=0` | Skips OpenCL **host** recovery only (the block after detectors). |
| `RETDEC_TYPE_INFERENCE` | Extra type-inference loop; **off by default**. |
| `--disable-static-code-detection` | Disables YARA / statically-linked-code signatures (`setIsDetectStaticCode(false)`). Does **not** disable algo / sort / container / concurrency detectors. |
| `RETDEC_INCREMENTAL_CACHE` | Skips **unchanged** functions on re-runs; does not skip detectors on a cold run. |
| `RETDEC_NEURAL_REFINE` | Separate cost axis; leave unset when measuring detectors. |

Do not add a public `--skip-detectors` (or similar) flag unless a later task
explicitly scopes one.

## Procedure

Use a small gcc-O0 ELF from `tests/algorithm_recovery/corpus/` (or any ~14 KB
sample). Rebuild **only** `retdec-decompiler` after a local experiment — not LLVM.

### 1. Time with detectors (stock tree)

```bash
export RETDEC_PROFILE_JSON=auto
export RETDEC_INCREMENTAL_CACHE=0
export RETDEC_OCL_HOST=0
unset RETDEC_NEURAL_REFINE
./build/linux/src/retdec-decompiler/retdec-decompiler \
  tests/algorithm_recovery/corpus/bubblesort-gcc-O0 \
  -o /tmp/retdec-det-with.c
```

Read `analysis.detectors` (and total wall) from `/tmp/retdec-det-with.c.profile.json`.

### 2. Time without the detector block (local experiment)

In `src/retdec/retdec.cpp`, comment out or `#if 0` the block that starts at

```text
// --- 4–7. Per-function detectors (container, algo, sort, concurrency) ---
```

through semantic export / `maybeRefineDecompilerOutput` (leave the later
OpenCL host block and `maybeDumpProfileJson` in place). Rebuild
`retdec-decompiler` only. Repeat the same command to `/tmp/retdec-det-without.c`.

### 3. Compare

- Detector share ≈ `analysis.detectors` ms from step 1
- Pipeline share ≈ `pipeline_wall_ms` / LLVM pass timers (or total wall minus detectors)
- Cross-check: step-1 total wall − step-2 total wall should match `analysis.detectors` within noise

On the 2026-08-17 six-ELF note in PERFORMANCE.md, post-pipeline analysis was
already only **12–24 ms** vs **550–750 ms** of LLVM / bin2llvmir. Re-measure
after the stem-augment deletion; do not reuse withdrawn algorithm-recovery F1
figures as a cost proxy.
