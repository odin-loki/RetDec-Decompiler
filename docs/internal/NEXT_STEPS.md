# Next steps (optional)

**Read first:** [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md).

**v2.0.24** — automation steps 1–26 complete, GitHub Release ships Linux /
Windows / macOS, default-`.c` recompile is **216/216**. These are optional
follow-ups, not a second master plan.

Stock RetDec 5.0 compare is in `results/stock-retdec-docker-full.json` and
[BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md). Historical dumps:
[data/README.md](../../data/README.md).

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

## Remaining real findings

The 0/216 default-`.c` rate is **closed** (216/216 at run 276).
[UNFIXED_AUDIT_FINDINGS.md](UNFIXED_AUDIT_FINDINGS.md) is a running log; the
items that still stand are listed at the top of that file. Among them:

- Native arm64 Mach-O still decompiles to an empty file (`ctest-macos` prints it)
- `types_propagator.cpp` is in no `CMakeLists.txt`
- libc++ deprecates `char_traits` for `WideCharType` (`uint32_t`)
- `DerefOpExpr` inherits `UnaryOpExpr::getType()` (pointer, not pointee)
- `ctest-linux` / `ctest-windows` still build a named subset of suites
- Pipeline stages marked **hook** / **partial** in
  [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md)

Do not bump LLVM unless an explicitly scoped task says so.

## Not planned

- OSS-Fuzz 23k corpus
- Four-toolchain `retdec-support` regen
- Dual Windows/WSL Git
- Further LLVM pin change
- Developer ID / notarised macOS packages
