# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0 | Steps 1–26, Part 16 automation |
| v1.1.0 | Steps 27–33 scaffolds (performance, rellic, LIEF, neural tiers 4–5) |
| v1.2.0 | Step 10 corpus starter, step 8.4/8.8/20 neural, demo, crash corpus |

## v1.2.0 highlights

- 6-source algorithm recovery corpus (expand to 200+)
- Semantic context in neural prompts
- Model SHA verification + differential gate scaffold
- Full `run_benchmarks.sh` pipeline

## Remaining human-led

| Item | Notes |
|------|-------|
| Corpus scale-up | Add sources until 200+ binaries |
| retdec-support regen | Toolchain farm |
| Triton/D-Helix differential | Replace stdout-compare gate |
| rellic / LIEF / Retypd / SAILR / LLVM | Evaluation and migration |
