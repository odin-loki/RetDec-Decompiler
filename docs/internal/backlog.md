# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Shipped

| Version | Scope |
|---------|-------|
| v1.0.0 | Steps 1–26, Part 16 |
| v1.1.0 | Steps 27–33 scaffolds |
| v1.2.0 | Corpus starter, neural context/gates, demo |
| v1.3.0 | 200+ corpus, prediction extraction, Triton gate scaffold |
| v1.4.0 | Live F1 in CI, label normalization, algorithm recovery gate |
| v1.5.0 | **Full-corpus nightly**, parallel extract, Triton/LIEF eval scaffolds |

## v1.5.0

- `algorithm-recovery-nightly` workflow (weekly CI core; full corpus on dispatch)
- `run_algorithm_recovery_full.sh` — 216 binaries, `--jobs 4`, gate ≥ 180 decompiled
- `triton_diff_gate.py` — stdout/fuzz/triton modes
- `eval_lief.sh` — LIEF vs readelf section comparison scaffold

## Remaining (human-led)

| Item | Notes |
|------|-------|
| Raise F1 floor | Update baseline after first green nightly |
| retdec-support regen | Run on toolchain farm; update deps.cmake pins |
| Full Triton symbolic (D-Helix) | Extend `triton_diff_gate.py` with path exploration |
| rellic backend swap | Blocked on LLVM 8 unless migration proceeds |
| Retypd / SAILR / LLVM | Long-horizon migrations (steps 30–33) |
