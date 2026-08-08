# Internal backlog — see MASTER-UPGRADE-PLAN.md

## Steps 1–26 (v1.0.0) — shipped

| Step | Task | State |
|------|------|-------|
| 1–8 | Foundations + D7 | done |
| 9 | DecompileBench runner | done |
| 10 | Algorithm recovery corpus | scaffold + runner |
| 11–15 | Deps bumps | done |
| 16 | retdec-support regen script | scaffold |
| 17–21 | Neural + gates | tiers 1–3; gates partial |
| 22–23 | Offline + fuzz/sanitizers | done |
| 24 | Tier-1 algorithms | Andersen wired; Braun + Semi-NCA scaffold |
| 25–26 | Release + **SHIP** | **v1.0.0 tagged** |

## Part 16 automation

| Item | State |
|------|-------|
| 16.1 upgrade-dep.sh | done |
| 16.2 run_benchmarks.sh --compare | done |
| 16.3 benchmark regression gate | done (ci-smoke) |
| 16.4 doctor.sh extensions | done |
| 16.5 nightly report | done (perf-nightly artifact) |
| 16.6 backlog.md | this file |

## Post-ship roadmap (steps 27–33)

| # | Task | Executor | State |
|---|------|----------|-------|
| 27 | Performance (11.1–11.4) | C+/H | perf-nightly scheduled; parallelism TBD |
| 28 | rellic evaluation | H | not started |
| 29 | LIEF adoption | C+ | not started |
| 30 | Retypd | H | not started |
| 31 | SAILR | H | not started |
| 32 | Neural tiers 4–5 | H | not started |
| 33 | LLVM migration | H | not started |

Human-led blockers: algorithm-recovery corpus (step 10), retdec-support regen (step 16), full verification gates (step 20).
