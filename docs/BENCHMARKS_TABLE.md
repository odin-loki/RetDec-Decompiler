# Benchmark tables

- **Version:** 2.0.20
- **Stock image:** `remnux/retdec` (RetDec v5.0, commit `53e55b4`, 2022-12-08)
- **Generated:** 2026-08-16T02:31:00Z

Official Hub image `retdec/retdec:v5.0` does not exist.

## DecompileBench (stand-in corpus, 216 binaries)

| Metric | Fork | Stock RetDec 5.0 | Baseline |
|--------|------|------------------|----------|
| syntax_valid_rate | 1.0 | 1.0 | 1.0 |
| recompile_success_rate | 0.0 | 0.0 | 0.0 |
| coverage_equivalence_rate | — | — | — |
| mean_wall_s | 1.492 | 0.242 | — |
| decompiled | 216 | 216 | 216 |

Per optimisation level (both sides): syntax 100%, recompile 0% at O0/O2/O3.

Stock wall time is in-container `retdec-decompiler` only. Fork wall time is the host decompiler process (includes semantic export). On this small ELF stand-in set both emit C and neither recompiles.

## Algorithm recovery

| Profile | mean_f1 | mean_f1_raw | decompiled |
|---------|---------|-------------|------------|
| CI core (9) | 1.0 | 1.0 | 9 |
| Full corpus (216) | 1.0 | **1.0** | 216 |

`mean_f1` uses stem/label fallback; `mean_f1_raw` is detector-only. Stock RetDec has no label export — F1 is fork-only.

Artifacts: `results/decompilebench-tmp.json`, `results/stock-retdec-docker-full.json`, `results/algorithm-recovery-full.json`.

_Regenerate fork: `python3 tests/decompilebench/runner.py ...`. Stock: `py -3 scripts/run_stock_retdec_docker.py --profile full --skip-pull`._
