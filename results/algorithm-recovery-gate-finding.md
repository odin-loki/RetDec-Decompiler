# Official algorithm-recovery gate vs honest name-blind F1

`scripts/run_algorithm_recovery_ci.sh` gates name-blind **0.12**
(`--no-stem-fallback`). `scripts/run_algorithm_recovery_full.sh` gates
name-blind **0.05** (measured full-corpus **0.056**). Stem-era **0.95**
is not the product metric (`CI-01`). The `--stem-fallback` extract path
stays for tests (`CI-02`); it is not the CI gate.

| Artefact | mean F1 | What it is |
|----------|---------|------------|
| `results/algorithm-recovery-full.json` | **1.0** | Withdrawn stem-tuned score |
| Official full-corpus script gate | **0.05** | Name-blind floor under measured 0.056 |
| `results/algorithm-recovery-full-nameblind.json` | **0.056** | Honest name-blind full 216 |
| `results/algorithm-recovery-ci.json` | **0.126** | Honest name-blind ci-core 9 |

A 0.95 stem-era pass is **not** current product quality. Do not advertise 1.0.
