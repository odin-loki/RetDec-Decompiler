# results/

Live measurement contract for CI, ship checklist, and the v2.0.22 release
table. RetDec Imortek is a specification-extraction decompiler; these files
are the **committed** evidence for recompile rate and algorithm-recovery F1.

CI and `scripts/run_benchmarks.sh` read **this** directory, not `data/`.

## Committed

| File | Role |
|------|------|
| `decompilebench.json` | Live per-sample DecompileBench contract (wall_s, profile stages) |
| `decompilebench-ci-core.json` | CI-core (9) DecompileBench |
| `decompilebench-full.json` | Full (216) DecompileBench |
| `baseline-2026-08.json` | DecompileBench regression gate |
| `baseline-algorithm-recovery.json` | Algorithm-recovery regression gate |
| `stock-retdec-docker-ci-core.json` | Stock RetDec 5.0 (`remnux/retdec`), 9 binaries |
| `stock-retdec-docker-full.json` | Stock RetDec 5.0, 216 binaries |
| `compare-fork-vs-stock.md` | Live DecompileBench compare (ci-core) |
| `compare-fork-vs-stock-full.md` | Live DecompileBench compare (216) |
| `algorithm-recovery-ci.json` | Name-blind fork F1 on CI-core (9) |
| `algorithm-recovery-full.json` | Withdrawn stem-tuned F1 (216) — do not advertise |
| `algorithm-recovery-full-nameblind.json` | Honest name-blind F1 on 216 (B12/B13) |
| `algorithm-recovery-per-opt.md` | Per-opt headline table for the name-blind run |
| `algorithm-recovery-adversarial-b9.json` | Name-blind F1 on the B9 idiosyncratic set (18) |
| `algorithm-recovery-third-party-b10.json` | B10 third-party JSON |
| `b9-adversarial-positive.md` | B9 recall table |
| `corpus-build-recipe.md` | How the corpora are built (B16) |
| `b8-loop-negatives.md` / `.json` | Loop-containing B8 FP rate + A4 observation |
| `b8-negative-corpus.md` / `.json` / `b8-negative-predictions.json` | B8 negative corpus |
| `b10-third-party.md` | zlib 1.3.1 third-party name-blind result |
| `goto-optimizer-baseline.md` / `.json` | Q4 goto counts on ci-core O0/O2/O3 |
| `a4-calibration.md` / `.json` | Reported confidence vs empirical precision (not fitted) |
| `b6-rename-guard.md` / `.json` | Named vs hashed labels on ci-core 9 |
| `b7-name-evidence.md` | Symbol-name tag; concurrency excluded from headline |
| `algorithm-recovery-gate-finding.md` | Name-blind full-corpus 0.05 gate vs measured 0.056 |
| `detector-stage-ab.json` | Detector stage A/B |
| `e1-real-binary-smoke.json` | E1 smoke |

Algorithm-recovery F1 figures that depended on filename filters are
**withdrawn** (B1–B5). Name-blind ci-core remasure:
`results/algorithm-recovery-ci.json` `mean_f1_raw` **0.126**. Full 216
name-blind mean **0.056**. Do not advertise 1.0. Stock has no label export —
F1 is fork-only.

Buildable-C recompile on the 216 stand-in is the product number for native
output (`--buildable` default on). Wall-clock fork-vs-stock ratios across
Debug/WSL vs Release-in-Docker are **not a comparison**.

## Generated (gitignored)

`results/<git-sha>.json`, `*-tmp.json`. After a local run, move leftovers to
[`../data/archive/`](../data/README.md) if you want the working tree clean.

## Commands

Scripts that exist at the repo root:

```bash
bash scripts/run_benchmarks.sh --compare 2026-08 --gate --profile ci-core
bash scripts/regenerate_benchmark_tables.sh
```

Stock Docker (PowerShell; image `remnux/retdec`, not `retdec/retdec:v5.0`):

```powershell
py -3 scripts\run_stock_retdec_docker.py --profile ci-core --skip-pull
```
