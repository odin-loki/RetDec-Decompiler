# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0 | Steps 1–26, Part 16 |
| v1.1.0 | Steps 27–33 scaffolds |
| v1.2.0 | Corpus starter, neural context/gates, demo |
| v1.3.0 | 200+ corpus, prediction extraction, Triton gate scaffold |
| v1.4.0 | Live F1 in CI, label normalization, algorithm recovery gate |
| v1.5.0 | Full-corpus nightly, parallel extract, Triton/LIEF eval scaffolds |
| v1.6.0 | **Regression gate**, D-Helix mode, Retypd/SAILR/migration eval suite |

## v1.6.0

- Regression gate vs `baseline-algorithm-recovery.json`
- `migration_eval_suite.sh` — rellic, LIEF, Retypd, SAILR
- D-Helix differential gate (`--mode dhelix`)

## Remaining (human-led)

| Item | Notes |
|------|-------|
| Raise F1 floor | Run `update_algorithm_recovery_baseline.sh` after green nightly |
| retdec-support regen | Toolchain farm; `regenerate-retdec-support.sh` |
| Full Triton symbolic paths | Extend `gate_dhelix` with constraint solving |
| rellic backend swap | `RETDEC_ENABLE_RELLIC` — blocked on LLVM 8 |
| Retypd / LLVM migration | Steps 30, 33 — months-long |
