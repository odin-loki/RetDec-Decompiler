# RetDec public roadmap

**Dated 2026-09-19.** Shipped facts match CHANGELOG [2.0.24] (2026-09-18)
and `CMakeLists.txt` `VERSION 2.0.24`. This is a status note, not a calendar.

The internal register is [Plan.md](Plan.md). Claims status lives in
[docs/CLAIMS.md](docs/CLAIMS.md). Research topics (not sprint work) are in
[docs/future_directions.md](docs/future_directions.md).

## Product

RetDec Imortek is a **specification-extraction decompiler**: algorithm
recovery, semantic export, and optional offline neural refinement are the
product; recovered C is a supporting artefact. Built on Avast RetDec 5.0.
Dual AGPL-3.0+ / commercial (Imortek). Repo:
[odin-loki/RetDec-Decompiler](https://github.com/odin-loki/RetDec-Decompiler).
Release tag [v2.0.24](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.24).

## LLVM

LLVM hops are **Track 2** — see
[docs/internal/UNBLOCKED-MIGRATION.md](docs/internal/UNBLOCKED-MIGRATION.md).
The pin is upstream `llvm-project` **23.1.0**, fetched as the monorepo
tarball (`cmake/deps.cmake`). This roadmap does **not** schedule a further
bump. Never edit `deps/llvm/`.

## Output, GUI, neural, CUDA

- Native output stays **C**. The CLI rejects `--output-lang cpp` because
  there is no dedicated C++ writer. Managed inputs ignore `--output-lang`
  and use format-specific emitters. `--buildable` sidecars are **on by
  default**.
- Qt 6 GUI (`retdec-gui`) runs the same `retdec-decompiler` child as the
  CLI. macOS ships `RetDec.app` in the arm64 tarball (ad-hoc signed, not
  notarised).
- Optional neural refine is `RETDEC_ENABLE_LLAMACPP` at build and
  `RETDEC_NEURAL_REFINE` / `RETDEC_NEURAL_MODEL` at run. Compile gate is
  `cc`/`gcc -fsyntax-only`; the differential gate is not implemented.
  There is no in-tree `src/qwen3` and no CLI `--model` flag.
- `RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF** and is not linked from
  `src/retdec` (`C-CUDA-PIPE` withdrawn).

## Quality gates (not product quality)

On the 216-binary stand-in corpus (not the OSS-Fuzz paper set):

| Item | Current |
|------|---------|
| `--buildable` recompile | 216/216 (stock 0/216) |
| Default `.c` recompile | 216/216 (CC-01; stock 0/216) |
| Name-blind F1 (headline) | 0.056 (95% CI 0.034–0.083) |
| CI name-blind F1 floors | **0.12** ci-core / **0.05** full |

Those CI floors are regression gates. They are not product quality.
Stem-era F1 **0.95** / name-assisted **1.0** is not the product metric.
This fork does **not** pursue the OSS-Fuzz paper corpus or four-compiler
support regeneration.

## Packages (v2.0.24)

`release-installers.yml` builds **Windows + Linux + macOS**:

- Linux `retdec-2.0.24-linux-x64.tar.gz` (+ `install.sh` / `uninstall.sh`)
- macOS `retdec-2.0.24-macos-arm64.tar.gz` (`RetDec.app`; loose
  `install-macos.sh` so it does not collide with Linux `install.sh`)
- Windows `retdec-2.0.24-windows-x64-setup.exe` and
  `retdec-2.0.24-windows-x64-portable.zip` (intended names; the Windows
  job may still be running on a given tag)
- Keyless Sigstore `.sigstore.json` next to those blobs; Authenticode is
  not applied
- Linux AppImage only if `APPIMAGE=1` / dispatch `appimage: true`
  (default **off**)

## Human leftovers

Ops and legal only — not a scheduled release:

- GHCR package Public (anonymous pull still 401)
- Docker Hub `imortek/retdec` unpublished
- Authenticode for Windows installers (keyless Sigstore is attached)
- CLA-assistant as a **required** branch-protection check
- `LEG-04` Australian IP solicitor
