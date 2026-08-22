# Claims register (audit E7)

Every externally visible claim needs a demonstrating artefact. A claim
without one is `unpublished` or `withdrawn`. Generate marketing copy from
this table; do not treat README, the whitepaper, or CHANGELOG as evidence.

**Status:** `demonstrated` — artefact in tree · `unpublished` — claimed, no
artefact · `withdrawn` — published then retracted · `opt-in` — off unless
the named flag is set.

Do not treat `results/algorithm-recovery-*.json` or
`docs/BENCHMARKS_TABLE.md` algorithm rows as current. CHANGELOG `[2.0.21]`
(B4) withdrew those figures after filename-derived detection was removed.

| ID | Claim | Where claimed | Demonstrating artefact | Status |
|----|-------|---------------|------------------------|--------|
| C-DB-STOCK | Stock RetDec 5.0 (`remnux/retdec`) DecompileBench JSON exists for the stand-in corpus (CI-core and full) | `CHANGELOG.md` `[2.0.20]`; `docs/BENCHMARKS.md`; `results/README.md` | `results/stock-retdec-docker-ci-core.json`; `results/stock-retdec-docker-full.json` | demonstrated |
| C-DB-COMPARE | Fork vs stock DecompileBench wall/quality two-column compare | `CHANGELOG.md` `[2.0.21]`; `results/compare-fork-vs-stock.md`; `results/compare-fork-vs-stock-full.md` | ci-core 9/9 buildable tu_valid+recompile. Full 216: buildable **1.000**. Debug vs stock Release ~6.3× (ci-core) / ~5.9× (full). Raw `.c` 0. Do not advertise a stock speed win. | demonstrated |
| C-ALGO-F1 | Algorithm-recovery F1 as a product/marketing figure | `README.md`; `docs/BENCHMARKS_TABLE.md`; pre-B1 CHANGELOG | Withdrawn (B4). Name-blind ci-core **0.126**; full 216 name-blind mean **0.056** in `results/algorithm-recovery-full-nameblind.json`. Do not advertise 1.0. | withdrawn |
| C-B12 | Per-optimisation name-blind F1 as a headline | `RETDEC_AUDIT.md` B12; `docs/BENCHMARKS.md` | `results/algorithm-recovery-per-opt.md`; O0 0.050 / O2 0.059 / O3 0.059. Not a product F1. | demonstrated |
| C-B13 | Micro/macro F1, per-class P/R, bootstrap 95% CI, `n` | `RETDEC_AUDIT.md` B13 | `tests/algorithm_recovery/runner.py`; `results/algorithm-recovery-full-nameblind.json` (`micro`, `macro_f1`, `per_class`, `mean_f1_ci95_*`) | demonstrated |
| C-E1 | Detector labels survive a hashed rename of the input ELF | `RETDEC_AUDIT.md` E1 | `scripts/ci/run_e1_real_binary_smoke.sh`; `results/e1-real-binary-smoke.json`. No F1 gate. Empty detections allowed. | demonstrated |
| C-B6 | CI rename guard: scores unchanged under `$(sha256).bin` | `RETDEC_AUDIT.md` B6 | `scripts/ci/run_b6_rename_guard.sh`; `results/b6-rename-guard.md`. ci-core 9/9 identical. | demonstrated |
| C-B7 | Symbol-name hits tagged and excluded from headline F1 | `RETDEC_AUDIT.md` B7 | Concurrency, open-addressing, named-variant introsort/heapsort/mergesort, name-only partition swap, name-only unordered-map hash, name-only shared_ptr atomic, name-only list alloc, and name-only vector growth export tag `evidence:symbol_name`; extract skips them. Extract already drops `std::partition` labels. ci-core **0.126**; full 216 **0.056**. `results/b7-name-evidence.md`. Serial/unordered/self-recursion still untagged. | demonstrated |
| C-B8 | Negative corpus: 200+ binaries with no target-algorithm labels; any hit is a false positive | `RETDEC_AUDIT.md` B8 | `scripts/generate_negative_corpus.py`; `scripts/ci/run_b8_negative_corpus.sh`; `results/b8-negative-corpus.md` | demonstrated |
| C-B8-LOOP | Loop-containing negatives; FP rate on strlen/atoi/dfs/sort heuristics | `RETDEC_AUDIT.md` B8 leftover | `scripts/ci/run_b8_loop_negatives.sh`; `results/b8-loop-negatives.md`. After sort/ring-buffer/copy gates, FP **0.000**. Not a product figure. | demonstrated |
| C-B10 | Third-party binaries labelled from upstream source | `RETDEC_AUDIT.md` B10 | `scripts/build_third_party_corpus.sh` (zlib 1.3.1 pin); `results/b10-third-party.md`. crc32-only 2/2 decompiled, F1 **0.000**. crc+deflate timed out. Not coreutils/OpenSSL/SQLite. | demonstrated |
| C-B9 | Adversarial-positive recall on idiosyncratic implementations | `RETDEC_AUDIT.md` B9 | `scripts/ci/run_b9_adversarial_corpus.sh`; `results/b9-adversarial-positive.md`. Name-blind mean F1 **0.111** on 18 binaries. Not a product figure. | demonstrated |
| C-B11 | Frozen holdout as hashes only | `RETDEC_AUDIT.md` B11 | `tests/algorithm_recovery/holdout/source-hashes.json` (B9 sources). Not a Debian/third-party holdout. | demonstrated |
| C-B16 | Corpus build recipe | `RETDEC_AUDIT.md` B16 | `results/corpus-build-recipe.md`. Host gcc/clang; no pinned container digest. | demonstrated |
| C-Q4 | Existing goto-optimizer baseline measured before any SAILR port | `RETDEC_AUDIT.md` Q4 | `scripts/ci/run_q4_goto_baseline.sh`; `results/goto-optimizer-baseline.md`. ci-core gcc O0/O2/O3: mean **1.44** gotos (O0 still 0). | demonstrated |
| C-A6 | Fibonacci / LCS / Knapsack are not structural detections | `RETDEC_AUDIT.md` A6 | `src/algo_recover/idiom_detect.cpp`; `tests/algo_recover/algo_recover_test.cpp` (`NeverAssignsFibonacciLcsKnapsack`) | demonstrated |
| C-A9 | Design-pattern recovery is experimental heuristics, not a product claim | `RETDEC_AUDIT.md` A9 | `src/pattern_detect/pattern_detector.cpp` file comment | demonstrated |
| C-NEURAL | Optional offline neural refinement (`RETDEC_ENABLE_LLAMACPP` / `RETDEC_NEURAL_REFINE`); compile-gate does not execute C | `README.md`; `docs/NEURAL_REFINEMENT.md`; `docs/COMMERCIAL_WHITEPAPER.md` | `src/neural/gates.cpp` (`cc`/`gcc` argv `-fsyntax-only`); `tests/neural/mock_test.cpp`; `CHANGELOG.md` `[2.0.21]` Security | opt-in |
| C-NEURAL-DIFF | Differential gate compiles and runs decompiled C | `docs/NEURAL_REFINEMENT.md`; `docs/COMMERCIAL_WHITEPAPER.md`; older CHANGELOG | `src/neural/gates.cpp`: `RETDEC_NEURAL_DIFF_GATE` warns and skips; `CHANGELOG.md` `[2.0.21]` Security | withdrawn |
| C-NEURAL-ALLOW | Model SHA allowlist on load (`support/models.json`); unknown GGUF refused unless `RETDEC_NEURAL_ALLOW_UNVERIFIED` | `RETDEC_AUDIT.md` N6; `support/models.md` | `support/models.json`; `src/neural/model_verify.cpp`; `tests/neural/mock_test.cpp`. Empty allowlist refuses load. Neural itself stays off until `RETDEC_NEURAL_REFINE` | demonstrated |
| C-N11 | Prompt that exceeds `n_ctx` is refused, not silently truncated | `RETDEC_AUDIT.md` N11 | `src/neural/llama_inference.cpp` (`llama_n_ctx` vs tokenize count + `maxTokens`). Piece buffer retry. Context-budget refuse retries once with head/tail source (`summarizeFunctionSource`). SIGINT/SIGTERM and `RETDEC_NEURAL_DEADLINE_MS` abort generate. GUI Stop already terminate()s the child. | demonstrated |
| C-N12 | Repeat refine is served from a content-addressed cache | `RETDEC_AUDIT.md` N12 | `src/neural/refiner.cpp` (`RETDEC_NEURAL_CACHE_DIR`). Off unless set. Key is SHA-256 of model path/pin, prompt, tier, sampler. `CacheHitReusesAcceptedRefinement`. | opt-in |
| C-N14 | Neural unit tests cover gates, injection strip, context overflow, and manifest schema | `RETDEC_AUDIT.md` N14 | `tests/neural/mock_test.cpp`: spawn-call reject (`system`/`execv`/`ShellExecuteA`/`_popen`), string-literal strip, comment-body strip, context-budget retry, `ManifestSchemaHasRequiredKeys`, `EachTierHasDistinctInstruction`, `ConcurrentIndependentRefinesDoNotCrash`. C-source fixtures only; no adversarial-binary injection corpus. Does not claim llama.cpp is thread-safe. | demonstrated |
| C-N15 | Naming-tier GBNF rename map; applyJsonRenameMap skips C keywords | `RETDEC_AUDIT.md` N15 | `src/neural/refiner.cpp` (`namingRenameMapGbnf`, `applyJsonRenameMap`). C11 keywords and spawn-family idents rejected as rename sources/targets. Tests: `ApplyJsonRenameMapRejectsKeywordTarget`, `ApplyJsonRenameMapRejectsSpawnTarget`. Off unless `RETDEC_NEURAL_REFINE`. | opt-in |
| C-N16 | Neural refine prompt includes existing semantic context | `RETDEC_AUDIT.md` N16 | `src/neural/decompile_hook.cpp` (`serializeSemanticContext`): demangled name, start, declaration, real name, source file, wrapped name, `from_debug`, return type, parameters, `usedCryptoConstants`, detections, RTTI class names, callers/callees. Comments are not dumped. Crypto/serial stay unwired; no invented caller-buffer fields. Tests in `tests/neural/mock_test.cpp`. Off unless `RETDEC_NEURAL_REFINE`. | opt-in |
| C-N18 | Whole-program bottom-up refine over the call graph | `RETDEC_AUDIT.md` N18 | Call-graph **context only**: `serializeSemanticContext` emits `callers`/`callees` from `codeReferences`. Not a per-function topological refine pass (needs a C parser / N10). Test: `SerializesCallGraphFromCodeReferences`. | opt-in |
| C-CUDA-PIPE | CUDA/OpenCL acceleration in the default decompiler pipeline | Historical README / CHANGELOG CUDA backend; `RETDEC_AUDIT.md` II.4 | `CHANGELOG.md` `[2.0.21]` Removed; `docs/CUDA_CAPABILITIES.md`; `src/cuda_accel/README.md` — not linked from `src/retdec` | withdrawn |
| C-CUDA-OPTIN | Experimental `cuda_accel` / `opencl` exist; `RETDEC_ENABLE_CUDA_ACCEL` default OFF; unintegrated | `README.md` (GPU Acceleration); `docs/CUDA_CAPABILITIES.md`; `docs/COMMERCIAL_WHITEPAPER.md` | CMake option default; `src/cuda_accel/README.md` | opt-in |
| C-EMIT | Buildable sidecars (`.h`, `_stubs.c`, `.buildable.c`) next to output C | `include/retdec/retdec/semantic_recovery_export.h` | `src/retdec/semantic_recovery_export.cpp`; `tests/retdec/semantic_recovery_export_test.cpp`. Off unless `RETDEC_EMIT_BUILDABLE` is set (non-empty, not `0`) | opt-in |
| C-ARCH-UNIMP | SPARC, SystemZ, and XCore lifting are unimplemented | `docs/ARCHITECTURE_TARGETS.md`; `RETDEC_AUDIT.md` T5 | `src/capstone2llvmir/capstone2llvmir.cpp`: `createSparc` / `createSysz` / `createXcore` throw `GenericError` | demonstrated |
| C-LICENCE | Upstream Avast RetDec v5.0 MIT notice retained; Imortek modifications recorded | `CHANGELOG.md` `[2.0.21]` Legal | `docs/PROVENANCE.md`; `LICENSE-MIT` | demonstrated |
| C-FAST24 | Fast decompile preset ~24% faster, byte-identical output | Historical README (Qt 6 GUI) | none — claim removed from README | withdrawn |
| C-QWEN3-GPU | In-tree Qwen3 / FlashAttention stack accelerates decompilation | Historical `docs/architecture.md` / whitepaper | not wired into `src/retdec`; docs now say so | withdrawn |
| C-B15 | Result JSON provenance keys required | `RETDEC_AUDIT.md` B15 | `scripts/ci/verify_result_provenance.py` | demonstrated |
| C-A12 | Anchor-function selection is "biggest function" | `RETDEC_AUDIT.md` A12 (`retdec.cpp:894-906`) | Stale. Detectors run on every SSA function (`FnWorkItem`); those lines are the incremental cache. | demonstrated |

## How to use

1. Do not republish C-ALGO-F1 or C-CUDA-PIPE.
2. Do not fill C-DB-COMPARE until `results/compare-fork-vs-stock.md` exists
   and points at both stock JSON files and a fork run on the same corpus.
3. Opt-in rows are capabilities, not default-on product features.
4. Whitepaper and README must not outrun this register.
