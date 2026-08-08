# Benchmark tables (auto-generated)

- **Version:** 2.0.16
- **Commit:** `d1009beb048f405324ac9ea94f933484d9e41bc2`
- **Generated:** 2026-08-08T23:07:00Z

## DecompileBench (full corpus stand-in, 216 binaries)

| Metric | Fork | Stock RetDec 5.0 | Baseline |
|--------|------|------------------|----------|
| syntax_valid_rate | 1.0 | — | 1.0 |
| recompile_success_rate | 0.0 | — | 0.0 |
| coverage_equivalence_rate | — | — | — |
| mean_wall_s | 1.99 | — | — |

Per optimisation level: syntax 100%, recompile 0% at O0/O2/O3.

## Algorithm recovery

| Profile | mean_f1 | mean_f1_raw | decompiled |
|---------|---------|-------------|------------|
| CI core (9) | 1.0 | 1.0 | 9 |
| Full corpus (216) | 1.0 | **0.29** | 216 |

`mean_f1` uses stem/label fallback; `mean_f1_raw` is detector-only.

_Regenerate: `bash scripts/regenerate_benchmark_tables.sh`_
