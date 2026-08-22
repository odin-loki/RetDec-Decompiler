# Algorithm recovery — name-blind per-optimisation F1

Source: `results/algorithm-recovery-full-nameblind.json`  
Method: `scripts/extract_decompiler_predictions.py --no-stem-fallback` on the
stand-in 216 ELF corpus, scored by `tests/algorithm_recovery/runner.py`.

This is **not** a product F1. Do not advertise it as one. The checked-in
`algorithm-recovery-full.json` `mean_f1` **1.0** is the withdrawn stem-tuned
score. Official `scripts/run_algorithm_recovery_full.sh` still gates
`MIN_MEAN_F1=0.95`; that mismatch is a finding, not a silent gate change.

ci-core (9 gcc-O0 binaries) name-blind `mean_f1` remains ≈ **0.335**
(`results/algorithm-recovery-ci.json`). The full set is harder.

## Headline (216 binaries)

| Metric | Value |
|--------|-------|
| mean F1 (per-binary) | **0.124** |
| bootstrap 95% CI | 0.093 – 0.159 (n=216, 2000 resamples) |
| micro F1 | 0.174 (tp=77, fp=360, fn=373) |
| macro F1 | **0.075** |

## Per optimisation level

| Opt | n | mean F1 | 95% CI | micro F1 |
|-----|---|---------|--------|----------|
| O0 | 72 | **0.137** | 0.082 – 0.200 | 0.150 |
| O2 | 72 | **0.116** | 0.063 – 0.179 | 0.189 |
| O3 | 72 | **0.121** | 0.069 – 0.183 | 0.198 |

Degradation O0 → O2 is small on this detector set. That is honesty, not a
quality win: name-blind recall is low at every level.

## What this is not

- Not a CFG-aware structural score.
- Not calibrated confidence (A4). B8 false-positive rate is 0.000 on a
  loop-free negative set, so there is nothing to fit.
- Not an official CI pass of `MIN_MEAN_F1=0.95`.
