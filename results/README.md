# results/

Live measurement contract for CI, ship checklist, and the release table.

## Committed

| File | Role |
|------|------|
| `decompilebench.json` | Live per-sample DecompileBench contract (wall_s, profile stages) |
| `baseline-2026-08.json` | DecompileBench regression gate |
| `baseline-algorithm-recovery.json` | Algorithm-recovery regression gate |
| `stock-retdec-docker-ci-core.json` | Stock RetDec 5.0 (`remnux/retdec`), 9 binaries |
| `stock-retdec-docker-full.json` | Stock RetDec 5.0, 216 binaries |
| `algorithm-recovery-ci.json` | Name-blind fork F1 on CI-core (9) |
| `algorithm-recovery-full.json` | Withdrawn stem-tuned F1 (216) — do not advertise |
| `algorithm-recovery-full-nameblind.json` | Honest name-blind F1 on 216 (B12/B13) |
| `algorithm-recovery-per-opt.md` | Per-opt headline table for the name-blind run |
| `algorithm-recovery-adversarial-b9.json` | Name-blind F1 on the B9 idiosyncratic set (18) |
| `b9-adversarial-positive.md` | B9 recall table |
| `corpus-build-recipe.md` | How the corpora are built (B16) |
| `b8-loop-negatives.md` | Loop-containing B8 FP rate + A4 observation |
| `b10-third-party.md` | zlib 1.3.1 third-party name-blind result |
| `goto-optimizer-baseline.md` | Q4 goto counts on ci-core O0/O2/O3 |
| `a4-calibration.md` | Reported confidence vs empirical precision (not fitted) |
| `b6-rename-guard.md` | Named vs hashed labels on ci-core 9 |
| `algorithm-recovery-gate-finding.md` | Official 0.95 gate vs honest 0.107 |

Algorithm-recovery F1 figures that depended on filename filters are
**withdrawn** (B1–B5). Name-blind ci-core remasure:
`results/algorithm-recovery-ci.json` `mean_f1` ≈ **0.335**. Do not
advertise 1.0. Stock has no label export — F1 is fork-only.

Live DecompileBench compare: `compare-fork-vs-stock.md` and
`decompilebench-ci-core.json`.

## Generated (gitignored)

`results/<git-sha>.json`, `*-tmp.json`. After a local run, move leftovers to
[`../data/archive/`](../data/README.md) if you want the working tree clean.

## Commands

```bash
bash scripts/run_benchmarks.sh --compare 2026-08 --gate --profile ci-core
bash scripts/regenerate_benchmark_tables.sh
```
