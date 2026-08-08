# Performance guide

Post-ship performance features (MASTER-UPGRADE-PLAN Part 11, step 27).

## Environment flags

| Variable | Default | Effect |
|----------|---------|--------|
| `RETDEC_PARALLEL_ANALYSIS` | on when `hardware_concurrency > 2` | Parallel container/sort/algo detectors (≥5 functions) |
| `RETDEC_INCREMENTAL_CACHE` | on | Load/save `.retdec-fn-cache.json` sidecar |
| `RETDEC_NEURAL_BATCH` | off | Batch neural refinement requests (scaffold) |
| `RETDEC_NEURAL_TIER_MAX` | `3` | Max refinement tier (1–5) |

## Incremental function cache

When decompiling to `output.c`, analysis results are cached in
`output.retdec-fn-cache.json`. Unchanged functions (matched by body hash) are
skipped on re-runs — useful for GUI iterative workflows and large binaries.

Disable with `RETDEC_INCREMENTAL_CACHE=0`.

## Parallel analysis

Post-LLVM detectors run in a `ThreadPool` when:

- `RETDEC_PARALLEL_ANALYSIS` is enabled (default on multi-core hosts)
- The module has at least five functions

LLVM IR construction remains single-threaded (`LLVMContext` is not thread-safe).

## Parallel batch decompile

`parallelBatchDecompile()` in `retdec.h` decompiles multiple input configs in
parallel (one config per binary). Use for corpus benchmarks.

## Profiling

- **CI:** `perf-nightly.yml` runs weekly (Sunday 03:00 UTC).
- **Local flame graph (Linux):** `bash scripts/flamegraph_profile.sh <binary>`
- **Nightly report:** `bash scripts/nightly_report.sh`

## CUDA

CUDA acceleration is **optional** and **off by default in CI** (`RETDEC_ENABLE_CUDA_ACCEL=OFF`).
Post-LLVM analysis detectors (container, sort, algo) run on CPU; see
[GUI_PHASE_D.md](internal/GUI_PHASE_D.md) for the product decision.

Neural inference may use llama.cpp CUDA when enabled at build time.

## Roadmap

- Neural batch decode via llama.cpp (11.4)
- LLVM-context-per-worker for full pipeline parallelism (11.2)
- rellic / LIEF adoption may shift bottlenecks (Part 13)
