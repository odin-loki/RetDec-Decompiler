# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0–v1.6.0 | Steps 1–33 scaffolds, CI, corpus, F1, migration evals |
| **v1.7.0** | **Plan completion**, LLVM inventory, release benchmark tables |

## Automation complete

All Composer-automatable MASTER-UPGRADE-PLAN items are done. See `docs/internal/PLAN_COMPLETION.md`.

## Human-led only

| Item | Notes |
|------|-------|
| Decision D7 | Product positioning — blocks README/whitepaper final |
| retdec-support regen | Toolchain farm; `regenerate-retdec-support.sh` stages output |
| rellic / Retypd / LLVM backends | Months-long; eval scaffolds in place |
| Profile-driven perf | Use `flamegraph_profile.sh` + `perf-nightly` artifacts |
