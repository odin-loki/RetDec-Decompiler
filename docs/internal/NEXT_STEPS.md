# Next steps (optional)

**Read first:** [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md) — what is actually
required vs what the MASTER plan listed for upstream parity.

## Shippable product: done

v2.0.19 — automation steps 1–26 complete. No Docker, no four-compiler farm,
no dual Git setup needed.

```bash
bash scripts/automation_status.sh
bash scripts/run_all_automation.sh --skip-support-regen
```

## Git and CI (optional)

Use **Windows PowerShell only** for Git and GitHub CLI:

```powershell
gh auth login
.\scripts\dispatch_algorithm_recovery_nightly.ps1          # CI core
.\scripts\dispatch_algorithm_recovery_nightly.ps1 -FullCorpus # 216 binaries on GitHub runners
gh run list --workflow=algorithm-recovery-nightly
```

## Re-run F1 after detector changes

Build in WSL (optional), commit/push from Windows:

```bash
bash scripts/wsl_build_decompiler.sh
bash scripts/run_algorithm_recovery_full.sh \
  --decompiler build/linux/src/retdec-decompiler/retdec-decompiler --jobs 4
bash scripts/update_algorithm_recovery_baseline.sh \
  --from results/algorithm-recovery-full.json --profile full_corpus
```

## Not planned for this fork

- Docker / `run_stock_retdec_docker.sh`
- OSS-Fuzz 23k DecompileBench corpus
- `regenerate-retdec-support.sh` (four toolchains)
- rellic / retypd / LLVM migration unless you start a dedicated spike

See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md) for rationale.
