# Maintainer scope (honest)

**v2.0.22.** This fork is shippable without a four-compiler toolchain farm or
dual Windows/WSL Git. Docker is used **only** to pull a published stock RetDec
image for the two-column DecompileBench table.

GitHub Release assets (tag `v2.0.22`): Linux x64 tarball; macOS arm64 tarball
plus `RetDec.app`; Windows NSIS `setup.exe` and portable zip from the
`windows-installer` job (decoupled from the ubuntu `release` job so a queued
Linux runner cannot block the Windows zip).

## What is done (v2.0.22)

- Algorithm recovery: name-blind **mean_f1_raw = 0.056** on the 216-binary
  stand-in corpus; stem-era 1.0 withdrawn
- Default-`.c` recompile **216/216** (CC-01, run 276). Stock RetDec 5.0 remains
  **0/216** and was not re-measured
- CI core (9 binaries): name-blind **0.126**
- DecompileBench on the **same stand-in corpus** (not OSS-Fuzz)
- Stock RetDec **v5.0** compare via `remnux/retdec`
  (see `results/stock-retdec-docker-full.json`)
- Doctor, ship checklist, baselines, release tags, LIEF adapter verify
- CI: `ctest-linux`, `ctest-macos` (push/PR), `ctest-windows`
  (schedule / `workflow_dispatch`), `ci-smoke`, `standalone-check`, `fuzz-pr`,
  `codeql`, `doc-integrity`, `verify-esbmc`, `release-installers`
  (linux + windows + macos)
- MAC-01 (`scripts/ci/check_macos_bundle.py`) for a relocatable Qt bundle; GUI
  tests use `QT_QPA_PLATFORM=cocoa` on macOS
- MSVC: `/STACK:67108864` on `retdec-decompiler`; u128 operators in
  `idiom_reconstruct/division_recovery.cpp`; corpus fixtures
  `/ENTRY:main /NODEFAULTLIB` + `msvc_fixture_printf.c`; longer CLI-diagnostics
  timeout on Windows
- `APInt::isPowerOf2` / `logBase2` instead of `getZExtValue` on i128
  (pow2 optimizer)
- LLVM pin **unchanged** at llvm-project **23.1.0** (`cmake/deps.cmake`). No
  bump unless an explicitly scoped task says so
  ([LLVM_MIGRATION_SCOPE.md](LLVM_MIGRATION_SCOPE.md))
- GUI v3 shipped (Qt 6). Product positioning is specification extraction
  ([D7_DECISION.md](D7_DECISION.md))

The stand-in corpus is sufficient for regression gates. It is not the
23k-function DecompileBench paper corpus.

## Git: one environment only

Use **Windows native Git** from PowerShell for all `git` and `gh` operations.
WSL is for the Linux decompiler build and for `gcc` scoring. Do not maintain
parallel Git credentials in WSL.

```powershell
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;" + $env:PATH
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

Official Hub image `retdec/retdec:v5.0` **does not exist**. We use
`remnux/retdec` (RetDec v5.0, commit `53e55b4`, 2022-12-08).

## Out of scope

| Item | Why we skip it |
|------|----------------|
| **OSS-Fuzz full DecompileBench** (23k functions) | oss-fuzz farm / image builds. Stand-in 216-binary corpus is enough. |
| **retdec-support regeneration** (MSVC + GCC + Clang + MinGW) | Upstream tarball in `cmake/deps.cmake` is fine. |
| **rellic / retypd / further LLVM bump** | Multi-month research. Pin stays 23.1.0 unless tasked. |
| **LIEF FormatFactory cutover** | Optional. Adapter tests already pass. |
| **Developer ID / notarised macOS** | MAC-01 is ad-hoc codesign. Gatekeeper still quarantines a browser download; `install.sh` strips `com.apple.quarantine`. |
| **Native ARM64 Mach-O as a production target** | Host build on arm64 works; lifting a native arm64 Mach-O still yields empty output. Corpus is ELF. |

## Honest metric caveats

- Stem-era 1.0 is withdrawn. Name-blind `mean_f1_raw` is **0.056** (full) /
  **0.126** (ci-core) — detector-only on the stand-in corpus, not proof that
  structural IR detection is solved in production.
- Stock compare is **emit quality** (syntax / recompile / wall time). Stock has
  **no algorithm-label export**, so F1 is fork-only.
- On this ELF stand-in set the fork's default `.c` recompiles **216/216**.
  Stock is **0/216**. Do not write both sides as recompile 0%.

## See also

- [NEXT_STEPS.md](NEXT_STEPS.md)
- [backlog.md](backlog.md)
- [BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md)
- [results/README.md](../../results/README.md) — live numbers
- [data/README.md](../../data/README.md) — archived JSON / logs
