# Performance guide

Post-ship performance features (MASTER-UPGRADE-PLAN Part 11, step 27).

## Environment flags

| Variable | Default | Effect |
|----------|---------|--------|
| `RETDEC_PARALLEL_ANALYSIS` | on when `hardware_concurrency > 2` | Parallel container/sort/algo detectors (≥5 functions) |
| `RETDEC_INCREMENTAL_CACHE` | on | Load/save `.retdec-fn-cache.json` sidecar |
| `RETDEC_NEURAL_BATCH` | off | Reuse KV prefix across refinement requests |
| `RETDEC_NEURAL_TIER_MAX` | `3` | Max refinement tier (1–5) |
| `RETDEC_BIN2LLVMIR_DIAG` | off | Per-pass LLVM wall ms + pipeline / post-pipeline split |
| `RETDEC_PROFILE_JSON` | off | Write `<output>.profile.json` (`auto` next to `-o`) |
| `RETDEC_TYPE_INFERENCE` | off | Extra per-function type-inference loop (off = faster) |
| `RETDEC_NEURAL_REFINE` | off | Offline llama.cpp refine (needs GGUF + `RETDEC_ENABLE_LLAMACPP`) |
| `RETDEC_NEURAL_THINKING` | off | Qwen `/think` (slower; off = `/no_think`) |
| `RETDEC_NEURAL_MAX_TOKENS` | `512` | Cap generated tokens per tier |
| `RETDEC_NEURAL_THREADS` | llama default | llama.cpp generation threads |
| `RETDEC_NEURAL_CTX` | `4096` | llama.cpp context length |
| `RETDEC_NEURAL_N_BATCH` | `512` | Prompt decode chunk size |

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
- **Stage JSON:** `RETDEC_PROFILE_JSON=auto ./retdec-decompiler in.bin -o out.c`
- **Pass split:** `RETDEC_BIN2LLVMIR_DIAG=1 ./retdec-decompiler in.bin -o out.c`
- **Harness:** `python3 tests/decompilebench/runner.py --decompiler build/linux/src/retdec-decompiler/retdec-decompiler --corpus tests/decompilebench/corpus --out results/decompilebench.json`
- **Nightly report:** `bash scripts/nightly_report.sh`

Default quality/speed (measured 2026-08-17, WSL, six ~14 KB gcc-O0 ELFs):

- Process wall **~1.2 s**, RSS **~70 MB**.
- **`pipeline_wall_ms` 550–750** (LLVM / bin2llvmir). Hottest: `instcombine`,
  `retdec-provider-init`, `retdec-llvmir2hll`, `retdec-decoder`.
- **`post_pipeline_analysis_wall_ms` 12–24**. Detectors ~8 ms, SSA rebuild
  ~3 ms, type inference **~0.5–2 ms when enabled**.
- `--profile balanced` and `--profile quality` match default wall on these
  samples. `--profile fast` drops passes (quality change).

Leave `RETDEC_TYPE_INFERENCE` and `RETDEC_NEURAL_REFINE` unset for the
fast default-quality path. Neural refine is a separate cost axis.

## CUDA

CUDA acceleration is **optional** and **off by default in CI** (`RETDEC_ENABLE_CUDA_ACCEL=OFF`).
Post-LLVM analysis detectors (container, sort, algo) run on CPU; see
[GUI_PHASE_D.md](internal/GUI_PHASE_D.md) for the product decision.

Neural inference may use llama.cpp CUDA when enabled at build time.

## Roadmap

- Speculative MTP decode when llama.cpp exposes a C API (b10451 has `load_mtp` only)
- LLVM-context-per-worker for full pipeline parallelism (11.2)
- Hottest remaining static cost: `instcombine` / provider-init / decoder / llvmir2hll
