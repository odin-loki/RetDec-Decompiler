# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0 | Steps 1–26, Part 16 |
| v1.1.0 | Steps 27–33 scaffolds |
| v1.2.0 | Corpus starter, neural context/gates, demo |
| v1.3.0 | 200+ corpus, prediction extraction, Triton gate scaffold |
| v1.4.0 | **Live F1 in CI**, label normalization, algorithm recovery gate |

## v1.4.0

- `ctest-linux` runs `run_algorithm_recovery_ci.sh` on 9-binary core subset
- Gate: ≥ 6 successful decompiles, mean F1 ≥ 0.0 (raise floor as detection improves)
- Label tests in ci-smoke

## Remaining (human-led)

| Item | Notes |
|------|-------|
| Raise F1 floor | Tune `algorithm_recovery_gate.sh --min-mean-f1` once baseline measured |
| Full-corpus F1 nightly | Extend perf-nightly or separate job (216 binaries) |
| retdec-support regen | Run on toolchain farm; update deps.cmake pins |
| Triton symbolic differential | Replace stdout-compare in `differential_gate_triton.sh` |
| rellic / LIEF / Retypd / SAILR / LLVM | Evaluation migrations |
