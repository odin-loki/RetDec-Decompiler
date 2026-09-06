# Next steps (optional)

**Read first:** [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md).

## Shippable product

v2.0.21 — automation steps 1–26 complete, plus stock RetDec 5.0 compare. Stock RetDec 5.0 compare is in
`results/stock-retdec-docker-full.json` and [BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md).
Historical dumps: [data/README.md](../../data/README.md).

```bash
bash scripts/automation_status.sh
bash scripts/run_all_automation.sh --skip-support-regen
```

## Re-run stock compare (Windows PowerShell)

```powershell
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;" + $env:PATH
py -3 scripts\run_stock_retdec_docker.py --profile ci-core --skip-pull
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

## Git and CI (optional)

```powershell
gh auth login
.\scripts\dispatch_algorithm_recovery_nightly.ps1 -FullCorpus
```

## Confirmed defects behind the LLVM build

[UNFIXED_AUDIT_FINDINGS.md](UNFIXED_AUDIT_FINDINGS.md) records audit findings
that could not be built or tested without the LLVM pin, so were written down
rather than attempted. The first entry is the single cause of the 0/216
default-`.c` recompile rate: `NoInitVarDefOptimizer` deletes every
initializer-less local declaration for C output, and the call-site comment
describes a use check the pass does not perform.

## Not planned

- OSS-Fuzz 23k corpus
- Four-toolchain `retdec-support` regen
- Dual Windows/WSL Git
