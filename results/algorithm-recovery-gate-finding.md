# Official algorithm-recovery gate vs honest name-blind F1

`scripts/run_algorithm_recovery_full.sh` still gates `MIN_MEAN_F1=0.95`.
That constant is the stem-era threshold. It was **not** silently lowered.

| Artefact | mean F1 | What it is |
|----------|---------|------------|
| `results/algorithm-recovery-full.json` | **1.0** | Withdrawn stem-tuned score |
| Official script gate | **0.95** | Still in force; will fail a name-blind run |
| `results/algorithm-recovery-full-nameblind.json` | **0.056** | Honest name-blind full 216 (was 0.107 before OA symbol-name tag) |
| `results/algorithm-recovery-ci.json` | **0.126** | Honest name-blind ci-core 9 (B7 dropped concurrency and OA) |

`scripts/run_algorithm_recovery_ci.sh` passes `--stem-fallback` so the
0.95 `mean_f1` gate matches the stem-era score it was written for.
`mean_f1_raw` stays name-blind. A 0.95 CI pass is **not** current
product quality. Do not advertise 1.0.
