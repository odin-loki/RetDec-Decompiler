# MASTER-UPGRADE-PLAN — automation completion status

Last updated: **v2.0.22**. Historical plan text stays in
[MASTER-UPGRADE-PLAN.md](MASTER-UPGRADE-PLAN.md).

## Steps 1–26 (shippable product)

| Area | Status | Artifacts |
|------|--------|-----------|
| CI smoke + ctest | Done | `ci-smoke.yml`; `ctest-linux.yml` (push/PR); `ctest-macos.yml` (push/PR); `ctest-windows.yml` (schedule / dispatch); `standalone-check.yml`; `fuzz-pr.yml`; `codeql.yml`; `doc-integrity.yml`; `verify-esbmc.yml` |
| DecompileBench | **Done (stand-in + stock v5.0)** | 216-binary harness; stock via `remnux/retdec`; OSS-Fuzz paper corpus out of scope |
| Algorithm recovery | Done (caveat) | name-blind **mean_f1_raw=0.056** (full) / **0.126** (ci-core); stem-era 1.0 withdrawn |
| Default `.c` recompile | Done | CC-01 **216/216** (run 276). Stock remains 0/216 |
| Neural refinement | Done | tiers 1–5, gates, llama.cpp pin, opt-in via env. Differential gate **not** implemented |
| Licensing | Done | AGPL + commercial, doctor checks |
| Fuzz / sanitizers | Done | `sanitizers.yml`, `tests/crash_corpus/` |
| Part 16 automation | Done | doctor, nightly report, benchmark gate, demo |
| Release | Done | `release-installers.yml`: linux-installer, **windows-installer** (no `needs: release`), macos-installer. Linux x64 tarball; macOS arm64 tarball + `RetDec.app`; Windows NSIS/zip |
| macOS bundle | Done | MAC-01 `check_macos_bundle.py`; GUI tests `QT_QPA_PLATFORM=cocoa` |
| MSVC / Windows | Done | `/STACK:67108864`; u128 ops; `/ENTRY:main /NODEFAULTLIB` fixtures; i128 `isPowerOf2`/`logBase2` |
| LLVM pin | Unchanged | llvm-project **23.1.0**. No bump unless tasked |

## Steps 27–33 (roadmap) — optional research

Scaffolds only. Not required to ship. See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md).

| Step | Item | Status |
|------|------|--------|
| 27 | Performance | Optional (C9 measurement exists; no stock-speed claim) |
| 28 | rellic | Optional spike |
| 29 | LIEF | Adapter done; FormatFactory cutover optional |
| 30–33 | Retypd / SAILR / further LLVM bump | Multi-month; not planned |

## Not pursuing (documented)

- OSS-Fuzz 23k paper corpus
- Four-toolchain `retdec-support` regeneration
- Dual Windows/WSL Git — **Windows Git + `gh` only**
- Building custom Docker images (we only pull `remnux/retdec`)
- Drive-by LLVM pin change

Optional: `gh auth login` (PowerShell) → `.\scripts\dispatch_algorithm_recovery_nightly.ps1`

See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md), [NEXT_STEPS.md](NEXT_STEPS.md), [backlog.md](backlog.md).
