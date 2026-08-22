# Algorithm recovery — name-blind per-optimisation F1

Source: `results/algorithm-recovery-full-nameblind.json`  
Method: `scripts/extract_decompiler_predictions.py --no-stem-fallback` on the
stand-in 216 ELF corpus, scored by `tests/algorithm_recovery/runner.py`.

This is **not** a product F1. Do not advertise it as one. The checked-in
`algorithm-recovery-full.json` `mean_f1` **1.0** is the withdrawn stem-tuned
score. Official `scripts/run_algorithm_recovery_full.sh` still gates
`MIN_MEAN_F1=0.95`; that mismatch is a finding, not a silent gate change.

Remasured after the detector precision gates (sort / hash / copy). Mean F1
dropped from 0.124 to **0.107** because false labels fell (fp 360 → 62)
and a few true labels were also dropped (tp 77 → 64). Do not advertise
the old 0.124 as current.

ci-core (9 gcc-O0 binaries) name-blind `mean_f1` is **0.237**
(`results/algorithm-recovery-ci.json`; CI95 0.037–0.496). The full
set is harder. hash_table is 1.000; pthread_mutex is 0.000 after
B7 excluded symbol-name concurrency hits.

## Headline (216 binaries)

| Metric | Value |
|--------|-------|
| mean F1 (per-binary) | **0.107** |
| bootstrap 95% CI | 0.073 – 0.145 (n=216, 2000 resamples) |
| micro F1 | 0.222 (tp=64, fp=62, fn=386) |
| macro F1 | **0.119** |

## Per optimisation level

| Opt | n | mean F1 | 95% CI | micro F1 |
|-----|---|---------|--------|----------|
| O0 | 72 | **0.102** | 0.045 – 0.165 | 0.202 |
| O2 | 72 | **0.110** | 0.050 – 0.179 | 0.232 |
| O3 | 72 | **0.110** | 0.050 – 0.179 | 0.234 |

Name-blind recall is low at every level. That is honesty, not a quality win.

## What this is not

- Not a CFG-aware structural score.
- Not calibrated confidence (A4). B8 extract FP is 0.000 on loop-negatives.
- Not an official CI pass of `MIN_MEAN_F1=0.95`.
