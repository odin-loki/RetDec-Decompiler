# Algorithm recovery — name-blind per-optimisation F1

Source: `results/algorithm-recovery-full-nameblind.json`  
Method: `scripts/extract_decompiler_predictions.py --no-stem-fallback` on the
stand-in 216 ELF corpus, scored by `tests/algorithm_recovery/runner.py`.

This is **not** a product F1. Do not advertise it as one. The checked-in
`algorithm-recovery-full.json` `mean_f1` **1.0** is the withdrawn stem-tuned
score. Official `scripts/run_algorithm_recovery_full.sh` still gates
`MIN_MEAN_F1=0.95`; that mismatch is a finding, not a silent gate change.

Remasured after B7 tagged open-addressing as `evidence:symbol_name`.
Mean F1 dropped from 0.107 to **0.056** because hash-table labels left
the headline (tp 64 → 34). Do not advertise 0.107 as current.

ci-core (9 gcc-O0 binaries) name-blind `mean_f1` is **0.126**
(`results/algorithm-recovery-ci.json`; CI95 0.000–0.341). hash_table
and pthread_mutex are both 0.000.

## Headline (216 binaries)

| Metric | Value |
|--------|-------|
| mean F1 (per-binary) | **0.056** |
| bootstrap 95% CI | 0.034 – 0.083 (n=216, 2000 resamples) |
| micro F1 | 0.126 (tp=34, fp=56, fn=416) |
| macro F1 | **0.049** |

## Per optimisation level

| Opt | n | mean F1 | 95% CI | micro F1 |
|-----|---|---------|--------|----------|
| O0 | 72 | **0.050** | 0.014 – 0.094 | 0.108 |
| O2 | 72 | **0.059** | 0.021 – 0.110 | 0.135 |
| O3 | 72 | **0.059** | 0.021 – 0.110 | 0.136 |

Name-blind recall is low at every level. That is honesty, not a quality win.

## What this is not

- Not a CFG-aware structural score.
- Not calibrated confidence (A4). B8 extract FP is 0.000 on loop-negatives.
- Not an official CI pass of `MIN_MEAN_F1=0.95`.
