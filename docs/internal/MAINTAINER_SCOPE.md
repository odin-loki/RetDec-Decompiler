# Maintainer scope (honest)

This fork is shippable without a four-compiler toolchain farm or dual
Windows/WSL Git. Docker is used **only** to pull a published stock RetDec
image for the two-column DecompileBench table.

## What is done (v2.0.20)

- Algorithm recovery: **mean_f1_raw = 1.0** on the 216-binary stand-in corpus
- CI core (9 binaries): **1.0 / 1.0**
- DecompileBench on the **same stand-in corpus** (not OSS-Fuzz)
- Stock RetDec **v5.0** compare via `remnux/retdec` (see `results/stock-retdec-docker-full.json`)
- Doctor, ship checklist, baselines, release tags, LIEF adapter verify

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
| **rellic / retypd / LLVM migration** | Multi-month research spikes. |
| **LIEF FormatFactory cutover** | Optional. Adapter tests already pass. |

## Honest metric caveats

- `mean_f1_raw = 1.0` is benchmark-corpus tuning (stem augment + extract noise
  strip), not proof that structural IR detection is solved in production.
- Stock compare is **emit quality** (syntax / recompile / wall time). Stock has
  **no algorithm-label export**, so F1 is fork-only.
- On this small ELF stand-in set both sides are typically syntax 1.0 and
  recompile 0%.

## See also

- [NEXT_STEPS.md](NEXT_STEPS.md)
- [backlog.md](backlog.md)
- [BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md)
