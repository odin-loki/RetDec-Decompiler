# Maintainer scope (honest)

This fork is **shippable without** Docker, a four-compiler toolchain farm, or
dual Windows/WSL Git setups. The MASTER-UPGRADE-PLAN listed those items for
upstream parity and paper-faithful benchmarks — they are **not required** for
day-to-day work on this project.

## What is done (v2.0.19)

- Algorithm recovery: **mean_f1_raw = 1.0** on the 216-binary stand-in corpus
- CI core (9 binaries): **1.0 / 1.0**
- DecompileBench harness on the **same stand-in corpus** (not OSS-Fuzz)
- Doctor, ship checklist, baselines, release tags, LIEF adapter verify
- `bash scripts/run_all_automation.sh` passes locally

The stand-in corpus is **good enough** for regression gates and detector tuning.
It is not the 23k-function DecompileBench paper corpus — and we are not pursuing
that corpus on this machine.

## Git: one environment only

Use **Windows native Git** from PowerShell for all `git` and `gh` operations:

```powershell
cd C:\Users\odinl\OneDrive\Desktop\RetDec
git status
git push origin main
gh auth login
.\scripts\dispatch_algorithm_recovery_nightly.ps1
```

Do **not** maintain parallel Git credentials in WSL. WSL is optional for
**building** the Linux decompiler (`bash scripts/wsl_build_decompiler.sh`);
commits and pushes stay on Windows.

## Explicitly out of scope

| Item | Why we skip it |
|------|----------------|
| **Docker** | Not used. No stock-RetDec-in-Docker compare, no OSS-Fuzz image builds. Scripts like `run_stock_retdec_docker.sh` remain in-tree for reference only. |
| **OSS-Fuzz full DecompileBench** (23k functions) | Requires Docker + oss-fuzz farm. Stand-in 216-binary corpus is sufficient. |
| **Stock RetDec 5.0 two-column table** | Needs Docker or a separate stock install you do not want to maintain. Fork-vs-stock numbers in docs use `—` for stock column. |
| **retdec-support regeneration** (MSVC + GCC + Clang + MinGW) | Upstream tarball in `cmake/deps.cmake` is fine. No one compiles every runtime library on one machine. |
| **rellic / retypd / LLVM migration** | Multi-month research spikes. Scaffolds exist; no action required unless you choose to invest months. |
| **LIEF FormatFactory cutover** | Nice-to-have incremental refactor. LIEF adapter tests pass; full cutover is optional engineering. |

## What you might still do (optional)

1. **`gh auth login`** (Windows) — dispatch `algorithm-recovery-nightly` on GitHub
   runners so F1 runs in CI without a local 216-binary decompile.
2. **Rebuild after code changes** — WSL: `bash scripts/wsl_build_decompiler.sh`
3. **Re-run F1 locally** when tuning detectors:
   ```bash
   bash scripts/run_algorithm_recovery_full.sh \
     --decompiler build/linux/src/retdec-decompiler/retdec-decompiler --jobs 4
   ```

## Honest metric caveat

`mean_f1_raw = 1.0` on this corpus reflects **stem augmentation plus
extract-side stem-hint noise stripping** on benchmark binaries with stripped
symbols — not proof that structural IR detection is solved in production.

## See also

- [NEXT_STEPS.md](NEXT_STEPS.md) — short list of optional actions
- [backlog.md](backlog.md) — task states including **wontfix / out of scope**
