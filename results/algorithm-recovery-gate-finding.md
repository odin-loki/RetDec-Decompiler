# Official algorithm-recovery gate vs honest name-blind F1

`scripts/run_algorithm_recovery_full.sh` still gates `MIN_MEAN_F1=0.95`.
That constant is the stem-era threshold. It was **not** silently lowered.

| Artefact | mean F1 | What it is |
|----------|---------|------------|
| `results/algorithm-recovery-full.json` | **1.0** | Withdrawn stem-tuned score |
| Official script gate | **0.95** | Still in force; will fail a name-blind run |
| `results/algorithm-recovery-full-nameblind.json` | **0.124** | Honest name-blind full 216 |
| `results/algorithm-recovery-ci.json` | **0.335** | Honest name-blind ci-core 9 |

This mismatch is a **finding**. Do not treat a 0.95 CI pass as current
product quality. Do not advertise 1.0.
