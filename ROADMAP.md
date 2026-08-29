# RetDec public roadmap

**Dated 2026-08-29.** Shipped facts match CHANGELOG [2.0.21] (2026-08-17).
This is a status note, not a calendar.

The internal register is [Plan.md](Plan.md). Claims status lives in
[docs/CLAIMS.md](docs/CLAIMS.md). Research topics (not sprint work) are in
[docs/future_directions.md](docs/future_directions.md).

## LLVM

LLVM hops are **Track 2** — see
[docs/internal/UNBLOCKED-MIGRATION.md](docs/internal/UNBLOCKED-MIGRATION.md).
The pin stays the Avast LLVM 8-era archive in `cmake/deps.cmake`. This
roadmap does **not** schedule a pin bump. Never edit `deps/llvm/`.

## Output and neural

- Native output stays **C**. The CLI rejects `--output-lang cpp` until
  `LLVM-22`; `cxx_backend` is unwired.
- Optional neural refine is `RETDEC_NEURAL_REFINE` (and
  `RETDEC_NEURAL_MODEL`). There is no in-tree `src/qwen3`.

## Quality gates (not product quality)

On the 216-binary stand-in corpus (not the OSS-Fuzz paper set):

| Item | Current |
|------|---------|
| `--buildable` recompile | 216/216 (stock 0/216) |
| Default `.c` recompile | 0/216 |
| Name-blind F1 (headline) | 0.056 |
| CI name-blind F1 floors | **0.12** ci-core / **0.05** full |

Those CI floors are regression gates. They are not product quality.
Stem-era F1 **0.95** / name-assisted **1.0** is not the product metric.
This fork does **not** pursue the OSS-Fuzz paper corpus or four-compiler
support regeneration.

## Human leftovers

Ops and legal only — not a scheduled release:

- GHCR package Public (anonymous pull still 401)
- Docker Hub `imortek/retdec` unpublished
- Authenticode for Windows installers (keyless Sigstore is attached)
- CLA-assistant as a **required** branch-protection check
- `LEG-04` Australian IP solicitor
