# Next steps (optional)

**Read first:** [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md).

## Shippable product

v2.0.20 — automation steps 1–26 complete, plus stock RetDec 5.0 compare. Stock RetDec 5.0 compare is in
`results/stock-retdec-docker-full.json` and [BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md).

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

## Not planned

- OSS-Fuzz 23k corpus
- Four-toolchain `retdec-support` regen
- Dual Windows/WSL Git
