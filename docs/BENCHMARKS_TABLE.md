# Benchmark tables

- **Version:** 2.0.21
- **Stock image:** `remnux/retdec` (RetDec v5.0, commit `53e55b4`, 2022-12-08)
- **Generated:** 2026-08-16T02:31:00Z (wall-time rows); quality rows updated 2026-08-25

Official Hub image `retdec/retdec:v5.0` does not exist.

## DecompileBench (stand-in corpus, 216 binaries)

Headline quality metric is **buildable C** (`--buildable`, default on), not default `.c`.

| Metric | Fork | Stock RetDec 5.0 |
|--------|------|------------------|
| Recompile, `--buildable` sidecar | **216/216** | **0/216** |
| Recompile, default `.c` | 0/216 | 0/216 |
| syntax_valid_rate (default `.c`) | 1.0 | 1.0 |
| mean_wall_s | 1.492 | 0.242 |

`mean_wall_s` is **not a comparison**: fork is Debug on WSL; stock is
Release inside Docker. Treat the ~6× ratio as unmeasured until both sides
are Release on the same hardware and container.

Default `.c` does not recompile on either side. Per optimisation level
(default `.c`): syntax 100%, recompile 0% at O0/O2/O3.

Artifacts: `results/compare-fork-vs-stock-full.md`,
`results/stock-retdec-docker-full.json`.

## Algorithm recovery (name-blind is the headline)

| Profile | mean F1 (name-blind) | 95% CI | n |
|---------|----------------------|--------|---|
| Full corpus | **0.056** | 0.034 – 0.083 | 216 |
| CI core | 0.126 | — | 9 |

Name-assisted (symbolicated binaries, stem/label fallback) scores 1.0 on
this corpus because function names match the labels. That is a **second
mode**, not the headline. Do not advertise 1.0.

The withdrawn stem-tuned `mean_f1` column is not published here.

Per-opt name-blind: O0 0.050 / O2 0.059 / O3 0.059
(`results/algorithm-recovery-per-opt.md`).
Stock RetDec has no label export — F1 is fork-only.

Artifacts: `results/algorithm-recovery-full-nameblind.json`.
Temporary harness dumps belong in `data/archive/`.

_Regenerate fork: `python3 tests/decompilebench/runner.py ...`. Stock: `py -3 scripts/run_stock_retdec_docker.py --profile full --skip-pull`._
