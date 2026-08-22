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
| C-DB-COMPARE | Fork vs stock DecompileBench wall/quality two-column compare | `CHANGELOG.md` `[2.0.21]`; `results/compare-fork-vs-stock.md` | Warm Debug fork mean 1.393 s vs stock Release 0.249 s (5.6×). Buildable `tu_valid` 0.333 vs raw 0. Both `recompile_success` 0. Do not advertise a stock speed win. | demonstrated |
| C-ALGO-F1 | Algorithm-recovery F1 (any published figure, including corpus-tuned `mean_f1_raw`) | `README.md` (Benchmarks); `docs/BENCHMARKS_TABLE.md`; `docs/COMMERCIAL_WHITEPAPER.md`; `results/README.md`; pre-`[2.0.21]` CHANGELOG | `CHANGELOG.md` `[2.0.21]` Removed (B4): figures withdrawn; `results/` must be regenerated | withdrawn |
| C-NEURAL | Optional offline neural refinement (`RETDEC_ENABLE_LLAMACPP` / `RETDEC_NEURAL_REFINE`); compile-gate does not execute C | `README.md`; `docs/NEURAL_REFINEMENT.md`; `docs/COMMERCIAL_WHITEPAPER.md` | `src/neural/gates.cpp` (`cc`/`gcc` argv `-fsyntax-only`); `tests/neural/mock_test.cpp`; `CHANGELOG.md` `[2.0.21]` Security | opt-in |
| C-NEURAL-DIFF | Differential gate compiles and runs decompiled C | `docs/NEURAL_REFINEMENT.md`; `docs/COMMERCIAL_WHITEPAPER.md`; older CHANGELOG | `src/neural/gates.cpp`: `RETDEC_NEURAL_DIFF_GATE` warns and skips; `CHANGELOG.md` `[2.0.21]` Security | withdrawn |
| C-NEURAL-ALLOW | Model SHA allowlist on load (`support/models.json`); unknown GGUF refused unless `RETDEC_NEURAL_ALLOW_UNVERIFIED` | `RETDEC_AUDIT.md` N6; `support/models.md` | `support/models.json`; `src/neural/model_verify.cpp`; `tests/neural/mock_test.cpp`. Empty allowlist refuses load. Neural itself stays off until `RETDEC_NEURAL_REFINE` | demonstrated |
| C-CUDA-PIPE | CUDA/OpenCL acceleration in the default decompiler pipeline | Historical README / CHANGELOG CUDA backend; `RETDEC_AUDIT.md` II.4 | `CHANGELOG.md` `[2.0.21]` Removed; `docs/CUDA_CAPABILITIES.md`; `src/cuda_accel/README.md` — not linked from `src/retdec` | withdrawn |
| C-CUDA-OPTIN | Experimental `cuda_accel` / `opencl` exist; `RETDEC_ENABLE_CUDA_ACCEL` default OFF; unintegrated | `README.md` (GPU Acceleration); `docs/CUDA_CAPABILITIES.md`; `docs/COMMERCIAL_WHITEPAPER.md` | CMake option default; `src/cuda_accel/README.md` | opt-in |
| C-EMIT | Buildable sidecars (`.h`, `_stubs.c`, `.buildable.c`) next to output C | `include/retdec/retdec/semantic_recovery_export.h` | `src/retdec/semantic_recovery_export.cpp`; `tests/retdec/semantic_recovery_export_test.cpp`. Off unless `RETDEC_EMIT_BUILDABLE` is set (non-empty, not `0`) | opt-in |
| C-ARCH-UNIMP | SPARC, SystemZ, and XCore lifting are unimplemented | `docs/ARCHITECTURE_TARGETS.md`; `RETDEC_AUDIT.md` T5 | `src/capstone2llvmir/capstone2llvmir.cpp`: `createSparc` / `createSysz` / `createXcore` throw `GenericError` | demonstrated |
| C-LICENCE | Upstream Avast RetDec v5.0 MIT notice retained; Imortek modifications recorded | `CHANGELOG.md` `[2.0.21]` Legal | `docs/PROVENANCE.md`; `LICENSE-MIT` | demonstrated |
| C-FAST24 | Fast decompile preset ~24% faster, byte-identical output | `README.md` (Qt 6 GUI) | none | unpublished |
| C-QWEN3-GPU | In-tree Qwen3 / FlashAttention stack accelerates decompilation | `docs/architecture.md`; `docs/COMMERCIAL_WHITEPAPER.md` | not wired into `src/retdec` | unpublished |

## How to use

1. Do not republish C-ALGO-F1 or C-CUDA-PIPE.
2. Do not fill C-DB-COMPARE until `results/compare-fork-vs-stock.md` exists
   and points at both stock JSON files and a fork run on the same corpus.
3. Opt-in rows are capabilities, not default-on product features.
4. Whitepaper and README must not outrun this register.
