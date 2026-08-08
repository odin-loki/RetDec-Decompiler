# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Steps 1–26 (v1.0.0) — shipped

| Step | Task | State |
|------|------|-------|
| 1–26 | Foundations through SHIP | **v1.0.0** |

## Part 16 automation — shipped in v1.0.0

Benchmark gate, doctor extensions, nightly report — all done.

## v1.1.0 post-ship scaffolds

| # | Task | State |
|---|------|-------|
| 27 | Performance 11.1–11.4 | docs + flamegraph; parallel/cache flags; neural batch scaffold |
| 28 | rellic evaluation | docs + `eval_rellic.sh` + `RETDEC_ENABLE_RELLIC` |
| 29 | LIEF adoption | `LiefAdapter` stub + docs + `RETDEC_ENABLE_LIEF` |
| 30 | Retypd | roadmap doc only |
| 31 | SAILR | roadmap doc only |
| 32 | Neural tiers 4–5 | `RETDEC_NEURAL_TIER_MAX=5`; compile gate |
| 33 | LLVM migration | roadmap doc only |

## Human blockers (unchanged)

- Algorithm-recovery corpus (step 10) — needs 200+ binaries + labels
- retdec-support regen (step 16) — needs toolchain farm
- Full differential verification gate (step 20) — needs Triton/D-Helix
