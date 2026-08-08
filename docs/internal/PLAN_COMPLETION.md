# MASTER-UPGRADE-PLAN — automation completion status

Last updated: v2.0.14

## Steps 1–26 (shippable product) — DONE

All Composer-automatable items through Step 26 are implemented or scaffolded.
Shipped releases v1.0.0–v2.0.14 on `main`.

| Area | Status | Artifacts |
|------|--------|-----------|
| CI smoke + ctest | Done | `ci-smoke.yml`, `ctest-linux.yml` |
| DecompileBench | Done | `tests/decompilebench/`, `run_benchmarks.sh` |
| Algorithm recovery | Done | 216 corpus, CI + full corpus F1=1.0, nightly, regression gate |
| Neural refinement | Done | tiers 1–5, gates, llama.cpp pin, model SHA |
| Licensing | Done | AGPL + commercial, doctor checks |
| Fuzz / sanitizers | Done | `sanitizers.yml`, `tests/crash_corpus/` |
| Part 16 automation | Done | doctor, nightly report, benchmark gate, demo |
| Release | Done | `release-installers.yml`, tag-driven version |

## Steps 27–33 (roadmap) — SCAFFOLDED

| Step | Item | Automation | Human-led remainder |
|------|------|------------|---------------------|
| 27 | Performance | `perf-nightly.yml`, `flamegraph_profile.sh`, `PERFORMANCE.md` | Profile-driven optimizations |
| 28 | rellic | `eval_rellic.sh` | Build rellic on LLVM 8; backend swap |
| 29 | LIEF | `LiefAdapter` parseSections, `cmake/lief_optional.cmake`, `eval_lief.sh` | `RETDEC_ENABLE_LIEF=ON` + FormatFactory cutover |
| 30 | Retypd | `eval_retypd.sh` | 3–6 month spike |
| 31 | SAILR | `eval_sailr.sh`, `goto_cfg_optimizer` | Structure recovery integration |
| 32 | Neural tiers 4–5 | `RETDEC_NEURAL_TIER_MAX`, `BatchRefiner` | Frontier model review |
| 33 | LLVM migration | `inventory_llvm_apis.sh` | Retypd-first, one pass at a time |

## Nightly workflows

| Workflow | Schedule | Purpose |
|----------|----------|---------|
| `perf-nightly` | Weekly | Perf bench + CI-core F1 |
| `algorithm-recovery-nightly` | Weekly | F1 + regression + migration evals |
| `sanitizers` | Weekly | ASan over sample corpus |

## Human-led only

1. **retdec-support regen** — `regenerate-retdec-support.sh` stages; needs toolchain farm
2. **rellic / LLVM backend** — blocked on LLVM 8 alignment
3. **Retypd / SAILR production** — research timelines in MASTER-UPGRADE-PLAN Part 9
4. **GitHub nightly dispatch** — `gh auth login` then `bash scripts/dispatch_algorithm_recovery_nightly.sh [--full-corpus]`

## Decision D7 — CLOSED (v1.8.0)

**Positioning:** specification-extraction tool that contains a decompiler (option b).
Documented in [D7_DECISION.md](D7_DECISION.md). README and whitepaper aligned.

See [NEXT_STEPS.md](NEXT_STEPS.md) for human-led follow-ups.

## GUI roadmap — Phase D closed (v2.0.0)

CUDA hot-path and AI assistant decisions documented in [GUI_PHASE_D.md](GUI_PHASE_D.md).
