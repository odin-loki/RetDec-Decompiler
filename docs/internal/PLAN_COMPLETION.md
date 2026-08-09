# MASTER-UPGRADE-PLAN — automation completion status

Last updated: v2.0.14+ (measurement gap closure)

## Steps 1–26 (shippable product)

| Area | Status | Artifacts |
|------|--------|-----------|
| CI smoke + ctest | Done | `ci-smoke.yml`, `ctest-linux.yml` |
| DecompileBench | **Done (stand-in)** | 216-binary harness; OSS-Fuzz/Docker corpus **out of scope** |
| Algorithm recovery | Done (caveat) | **mean_f1_raw=1.0** on full corpus (v2.0.19 extract refinement; benchmark-tuned) |
| Neural refinement | Done | tiers 1–5, gates, llama.cpp pin, opt-in via env |
| Licensing | Done | AGPL + commercial, doctor checks |
| Fuzz / sanitizers | Done | `sanitizers.yml`, `tests/crash_corpus/` |
| Part 16 automation | Done | doctor, nightly report, benchmark gate, demo |
| Release | Done | `release-installers.yml`, tag-driven version |

## Steps 27–33 (roadmap) — optional research

Scaffolds only. Not required to ship. See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md).

| Step | Item | Status |
|------|------|--------|
| 27 | Performance | Optional |
| 28 | rellic | Optional spike |
| 29 | LIEF | Adapter done; FormatFactory cutover optional |
| 30–33 | Retypd / SAILR / LLVM | Multi-month; not planned |

## Not pursuing (documented)

- Docker (stock RetDec compare, OSS-Fuzz corpus)
- Four-toolchain `retdec-support` regeneration
- Dual Windows/WSL Git — **Windows Git + `gh` only**

Optional: `gh auth login` (PowerShell) → `.\scripts\dispatch_algorithm_recovery_nightly.ps1`

See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md), [NEXT_STEPS.md](NEXT_STEPS.md), [backlog.md](backlog.md).
