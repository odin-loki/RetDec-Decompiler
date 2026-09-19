# Due diligence register

Responses to the blocking findings in [`Plan.md`](../Plan.md) Part 2.
This file is the pre-empting artefact (`BIZ-04`). Phase 0 docs landed
2026-08-25; Phase 1 is largely in CHANGELOG [2.0.21]; packaging and
Windows/macOS verification in [2.0.22].

| ID | Finding | Response | Residual |
|----|---------|----------|----------|
| B1 | Public docs advertised withdrawn F1 1.0 and default-`.c` recompile 0 as the quality story | README **Results** and `docs/BENCHMARKS_TABLE.md` now lead with name-blind **0.056** (95% CI 0.034–0.083) and `--buildable` **216/216** vs stock **0/216**. Stem-fallback 1.0 is labelled a second mode, not the headline. `results/compare-fork-vs-stock-full.md` still records default `.c` `tu_valid` **0** from the DecompileBench JSON — cited, not deleted. | Nightly still runs `run_algorithm_recovery_ci.sh`; that script now gates name-blind **0.12** without `--stem-fallback` (`CI-01`). CC-01 live job also names a **252**-file corpus; that later rate was not re-measured here. |
| B2 | 151 upstream files lack the Avast copyright line | Rewrite years **2017–2020** restored under `src/`/`include/`/`tests/`. CI fails on leftover Odin Loch rewrite lines (`check_avast_mit_notice.py`). File-level classes in [`PROVENANCE-files.md`](PROVENANCE-files.md) from `scripts/ci/generate_provenance.py` (`LEG-03`): four classes only (`avast-mit` / `pelib-porst` / `imortek-or-undated` / `rewrite-tell-leftover`). | **178** Odin-only files live in upstream module directories (Imortek additions, not rewrite tells). Checker also scans `docs/doxygen/`. Plan.md's 151/129 figures are the pre-restore measurement. |
| B3 | No CLA; dual-licence relicensing is not wired | [CLA.md](../CLA.md) + PR template / CONTRIBUTING outbound grant (`LEG-05`). `.github/workflows/cla.yml` runs CLA-assistant on `pull_request_target` (`LEG-06`). Dual-licence files at **repo root**: `LICENSE`, `LICENSE-MIT`, `LICENSE-AGPL`, `LICENSE-COMMERCIAL`, `NOTICE`. | Making **CLA Assistant** a required branch-protection check is a GitHub UI setting. `LEG-04` solicitor still open. `C-LICENCE` stays **asserted**. |
| B4 | Detector confidence precision 0.000 (A4, n=160) | `results/a4-calibration.md` remains the measurement. README does not treat confidence as calibrated. Constants were not fitted. | Fitting is Phase 2+ (`DET` track). Do not advertise scores as probabilities. |
| B5 | Documented `retdec-qwen3-runner` / `--model` did not exist | Purged from README, user manual, Windows/MinGW docs, whitepaper. Neural path is `RETDEC_NEURAL_REFINE` + `RETDEC_NEURAL_MODEL`. `scripts/README.md` no longer advertises the phantom runner. `build-install-run-windows.ps1` defaults to `retdec-decompiler`. Differential neural gate is withdrawn (`C-NEURAL-DIFF`). | `run-qwen3-trace.ps1` remains as a leftover that errors; it is not a CMake target. |
| B6 | Incremental cache can return wrong results; on by default | `CACHE-01`: `computeFunctionBodyHash` now includes integer immediate operand values (`BodyHashDistinguishesConstantOperands`). Determinism and `RETDEC_INCREMENTAL_CACHE=0` tests. `ctest-linux` diffs cache-off vs cache-on on `fib_smoke` (`CACHE-05`). `CACHE-06`: sidecar `version` mismatch or missing field yields an empty cache. `CACHE-03`: body hash is SHA-256 via `fileformat::getSha256` (cache `kVersion` 3). | Detector-version / threshold-file tokens (`CACHE-02`) remain. Full 216-binary corpus differential is not yet gated. |
| B7 | README advertised eleven output languages; native path is C | README / architecture / whitepaper / user manual rewritten as input-keyed tables. `--output-lang cpp` throws (`C-CXX-EMIT` withdrawn). RISC-V is not implemented (`C-ARCH-UNIMP`). | Unwired emitters remain in-tree (`DEAD-02`, Phase 1B). Tests under `tests/fsharp_emitter` / `tests/vbnet_emitter` block deleting those emitters. |
| B8 | No git tags / no downloadable release | [QUICKSTART.md](../QUICKSTART.md) exists (`REL-07`). Tag path is `v2.0.22` (`CMakeLists.txt` / `releases/VERSION` / CHANGELOG). `release-installers.yml` publishes Linux tarball, Windows NSIS `setup.exe` + portable zip (`scripts/build-windows-installer.ps1` locally), **macOS** `retdec-*-macos-arm64.tar.gz` + `RetDec.app` (MAC-01), CycloneDX JSON, `fib_smoke` / `fib_smoke.exe`; signed blobs have keyless `.sigstore.json` (`C-SIGSTORE`). AppImage is **opt-in** (`APPIMAGE=1` / `appimage-from-release.yml`), not the default Linux artefact (`C-APPIMAGE`). `docker-from-release.yml` packs GHCR from the Linux tarball; unauthenticated pulls have returned 401. Docker Hub `imortek/retdec` is unpublished (`C-DOCKER-HUB`). | **Authenticode remains absent.** `docker pull imortek/retdec` is not live. macOS is ad-hoc codesign, not notarised. |
| B9 | AGPL is a procurement blocker for the stated buyer | Dual-licence text remains at repo root; commercial terms no longer publish a price list (`LEG-10`). [LICENSING_FAQ.md](../LICENSING_FAQ.md) answers air-gap / corresponding-source questions from the commercial text (`LEG-09`). | Keystone is excluded from commercial install by CMake and `check_keystone_not_installed.py` (`LEG-11`). Qt dynamic-link CMake gate is `check_qt_dynamic_link.py`; zip `dumpbin` is weekly (`qt-lgpl-evidence.yml`, default tag `v2.0.22`) (`LEG-12`). |

## Phase 0 exit (docs)

- No public document asserts `C-ALGO-F1`, `C-NEURAL-DIFF`, or `C-CXX-EMIT` as implemented product figures.
- No public document names `retdec-qwen3-runner` or CLI `--model` as shipped.
- `--buildable` / `RETDEC_EMIT_BUILDABLE` 216/216 vs stock 0/216 is above the fold in the README.
- Wall-clock Debug/WSL vs Release/Docker is marked unmeasured (`BENCH-06`).
- LLVM line in `NOTICE` is Apache-2.0-with-LLVM-exceptions (`LEG-13`).
- `DET-01` (`emittedAnnotation`) was **not** deleted: fourteen `tests/crypto_detect` cases assert on it.
- CI surfaces that Plan.md called out now exist: `ctest-macos`, `standalone-check`, `doc-integrity`, `verify-esbmc`, `fuzz-pr` (replay on PR; libFuzzer not on PR).

## Not in this pass

Phase 1 is largely landed in CHANGELOG [2.0.21]; 2.0.22 added the macOS
release, MAC-01, MSVC u128 / APInt isPowerOf2 / UCRT-free corpus fixtures.
Leftovers are tree-blocked or human/ops — not “Phase 1 has not started.”

`DEAD-02`/`DEAD-03` stay in-tree because emitter and GPU tests exist; do not
delete those tests. `CI-02` stem-fallback stays because
`tests/algorithm_recovery/test_labels.py` uses it. `DET-01` is blocked:
`tests/crypto_detect` asserts `emittedAnnotation`. `CACHE-02`/`CACHE-04`:
do not invent detector-version tokens or HMAC keys.

Human remaining: CLA as a required branch-protection check, GHCR package
Public, Docker Hub `imortek/retdec` unpublished, Authenticode, Apple
notarisation, `LEG-04` solicitor, OSS-Fuzz filing. LLVM pin is Track 2;
never edit `deps/llvm/`. `SAN-04` ASan on PRs exists (`sanitizers.yml` path
filter + `standalone-check` asan job); `SAN-02` TSan and `CI-11` 360-min
LLVM container are later. `CI-10` secret scanning/Dependabot already on.
ci-core name-blind 0.12 is live; full-corpus name-blind 0.05 is live
(`scripts/run_algorithm_recovery_full.sh`).
