# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0 | Steps 1–26, Part 16 |
| v1.1.0 | Steps 27–33 scaffolds |
| v1.2.0 | Corpus starter, neural context/gates, demo |
| v1.3.0 | **200+ corpus**, prediction extraction, Triton gate scaffold |

## v1.3.0

- 36 algorithm sources → 216+ stripped binaries (gcc+clang × O0/O2/O3)
- End-to-end: build → decompile → extract predictions → F1 metrics
- CI enforces corpus ≥ 200 on Linux

## Remaining (human-led)

| Item | Notes |
|------|-------|
| Live F1 on full corpus | Needs decompiler build in benchmark CI |
| retdec-support regen | Run on toolchain farm; update deps.cmake pins |
| Triton symbolic differential | Replace stdout-compare in `differential_gate_triton.sh` |
| rellic / LIEF / Retypd / SAILR / LLVM | Evaluation migrations |
