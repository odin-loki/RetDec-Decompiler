# Due diligence register

Responses to the blocking findings in [`Plan.md`](../Plan.md) Part 2.
This file is the pre-empting artefact (`BIZ-04`). Status is as of the
Phase 0 documentation pass (2026-08-25).

| ID | Finding | Response | Residual |
|----|---------|----------|----------|
| B1 | Public docs advertised withdrawn F1 1.0 and default-`.c` recompile 0 as the quality story | README **Results** and `docs/BENCHMARKS_TABLE.md` now lead with name-blind **0.056** (95% CI 0.034–0.083) and `--buildable` **216/216** vs stock **0/216**. Stem-fallback 1.0 is labelled a second mode, not the headline. | Nightly still runs `run_algorithm_recovery_ci.sh`; that script now gates name-blind **0.12** without `--stem-fallback` (`CI-01`). |
| B2 | 151 upstream files lack the Avast copyright line | Rewrite years **2017–2020** restored under `src/`/`include/`/`tests/`. CI fails on leftover Odin Loch rewrite lines (`check_avast_mit_notice.py`). File-level classes in [`PROVENANCE-files.md`](PROVENANCE-files.md) (`LEG-03`). | 129 Odin-only files live in upstream module directories (Imortek additions, not rewrite tells). |
| B3 | No CLA; dual-licence relicensing is not wired | [CLA.md](../CLA.md) + PR template / CONTRIBUTING outbound grant (`LEG-05`). | CLA-assistant required check (`LEG-06`) needs a GitHub app token. |
| B4 | Detector confidence precision 0.000 (A4, n=160) | `results/a4-calibration.md` remains the measurement. README does not treat confidence as calibrated. Constants were not fitted. | Fitting is Phase 2+ (`DET` track). Do not advertise scores as probabilities. |
| B5 | Documented `retdec-qwen3-runner` / `--model` did not exist | Purged from README, user manual, Windows/MinGW docs, whitepaper. Neural path is `RETDEC_NEURAL_REFINE` + `RETDEC_NEURAL_MODEL`. | None for the phantom binary. |
| B6 | Incremental cache can return wrong results; on by default | `CACHE-01`: `computeFunctionBodyHash` now includes integer immediate operand values (`BodyHashDistinguishesConstantOperands`). Determinism and `RETDEC_INCREMENTAL_CACHE=0` tests. `ctest-linux` diffs cache-off vs cache-on on `fib_smoke` (`CACHE-05`). `CACHE-06`: sidecar `version` mismatch or missing field yields an empty cache. `CACHE-03`: body hash is SHA-256 via `fileformat::getSha256` (cache `kVersion` 3). | Detector-version / threshold-file tokens (`CACHE-02`) remain. Full 216-binary corpus differential is not yet gated. |
| B7 | README advertised eleven output languages; native path is C | README / architecture / whitepaper / user manual rewritten as input-keyed tables. | Unwired emitters remain in-tree (`DEAD-02`, Phase 1B). Tests under `tests/fsharp_emitter` / `tests/vbnet_emitter` block deleting those emitters. |
| B8 | No git tags / no downloadable release | [QUICKSTART.md](../QUICKSTART.md) exists (`REL-07`). Tag `v2.0.21` (`REL-01`) fires `release-installers.yml`. CycloneDX of CMake pins attaches to the release (`REL-06`). | Published `imortek/retdec` image and cosign (`REL-02`, `REL-05`) remain. Windows/Linux installer artefacts depend on the tag workflow. |
| B9 | AGPL is a procurement blocker for the stated buyer | Dual-licence text remains; commercial terms no longer publish a price list (`LEG-10`). [LICENSING_FAQ.md](../LICENSING_FAQ.md) answers air-gap / corresponding-source questions from the commercial text (`LEG-09`). | Commercial packaging and Keystone exclusion (`LEG-11`) Phase 1A. |

## Phase 0 exit (docs)

- No public document asserts `C-ALGO-F1` or `C-NEURAL-DIFF` as implemented product figures.
- No public document names `retdec-qwen3-runner` or CLI `--model` as shipped.
- `--buildable` / `RETDEC_EMIT_BUILDABLE` 216/216 vs stock 0/216 is above the fold in the README.
- Wall-clock Debug/WSL vs Release/Docker is marked unmeasured (`BENCH-06`).
- LLVM line in `NOTICE` is Apache-2.0-with-LLVM-exceptions (`LEG-13`).
- `DET-01` (`emittedAnnotation`) was **not** deleted: fourteen `tests/crypto_detect` cases assert on it.

## Not in this pass

Phase 1 (legal headers, dead modules, cache hash, releases, CI truth) and
later phases. `DEAD-02`/`DEAD-03` stay in-tree because their tests exist.
`CI-02` stem-fallback deletion is blocked by `tests/algorithm_recovery/test_labels.py`.
`SAN-04` (ASan on every PR) is not the 360-minute weekly job. LLVM pin is Track 2.
