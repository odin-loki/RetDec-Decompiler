# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0–v1.7.0 | MASTER-UPGRADE-PLAN automation (steps 1–33 scaffolds) |
| **v1.8.0** | **D7 closed**, product docs alignment, ship checklist |

## v1.8.0

- **Decision D7 closed:** specification-extraction positioning (`docs/internal/D7_DECISION.md`)
- README, whitepaper, MASTER-UPGRADE-PLAN updated
- `ship_checklist.sh` — pre-release validation
- `demo.sh` — algorithm recovery + migration eval + ship checklist

## Human-led only

| Item | Notes |
|------|-------|
| retdec-support regen | Toolchain farm; `regenerate-retdec-support.sh` |
| rellic / Retypd / LLVM backends | Eval scaffolds in place; months-long integration |
| Profile-driven perf | `perf-nightly` + `flamegraph_profile.sh` |
