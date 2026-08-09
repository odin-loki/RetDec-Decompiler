# MASTER-UPGRADE-PLAN — automation completion status

Last updated: v2.0.14+ (measurement gap closure)

## Steps 1–26 (shippable product)

| Area | Status | Artifacts |
|------|--------|-----------|
| CI smoke + ctest | Done | `ci-smoke.yml`, `ctest-linux.yml` |
| DecompileBench | **Partial** | Harness + CI-core stand-in corpus; OSS-Fuzz full corpus not wired |
| Algorithm recovery | Done (caveat) | F1≈0.99 with fallback; **mean_f1_raw≈0.92** on full corpus (v2.0.18) |
| Neural refinement | Done | tiers 1–5, gates, llama.cpp pin, opt-in via env |
| Licensing | Done | AGPL + commercial, doctor checks |
| Fuzz / sanitizers | Done | `sanitizers.yml`, `tests/crash_corpus/` |
| Part 16 automation | Done | doctor, nightly report, benchmark gate, demo |
| Release | Done | `release-installers.yml`, tag-driven version |

## Steps 27–33 (roadmap) — SCAFFOLDED

| Step | Item | Automation | Human-led remainder |
|------|------|------------|---------------------|
| 27 | Performance | `perf-nightly.yml`, `flamegraph_profile.sh` | Profile-driven optimizations |
| 28 | rellic | `eval_rellic.sh` | Build rellic on LLVM 8 |
| 29 | LIEF | `install_lief_sdk.sh`, `LiefAdapter`, shadow mode | FormatFactory cutover |
| 30 | Retypd | `eval_retypd.sh` | 3–6 month spike |
| 31 | SAILR | `eval_sailr.sh` | Structure recovery integration |
| 32 | Neural tiers 4–5 | `RETDEC_NEURAL_TIER_MAX` | Frontier model review |
| 33 | LLVM migration | `inventory_llvm_apis.sh` | Retypd-first |

## Open engineering (automatable)

1. Run `bash scripts/run_benchmarks.sh --profile ci-core --fetch-stock` and commit baseline + `BENCHMARKS_TABLE.md`
2. Wire OSS-Fuzz DecompileBench corpus (Docker) for full 23k-function eval
3. `gh auth login` → dispatch `algorithm-recovery-nightly`
4. retdec-support regen on toolchain farm

See [NEXT_STEPS.md](NEXT_STEPS.md) and [backlog.md](backlog.md).
