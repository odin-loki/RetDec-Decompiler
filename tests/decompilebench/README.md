# DecompileBench harness (arXiv 2505.11340)

Measures pseudocode quality: syntax validity, recompile success, coverage
equivalence, wall time, and peak RSS.

RetDec Imortek v2.0.22 uses this harness on a **stand-in corpus**, not the
paper's OSS-Fuzz set. That is a maintainer-scope decision (no four-compiler
regen, no OSS-Fuzz paper corpus). Product positioning is specification
extraction; the headline native metric on this stand-in is **buildable C**
recompile, not wall-clock vs stock Docker.

## Corpus (this fork)

We use the **algorithm-recovery stand-in** — not the paper's OSS-Fuzz corpus.
See [OSS_FUZZ_SETUP.md](OSS_FUZZ_SETUP.md).

```bash
bash scripts/fetch_decompilebench_corpus.sh --profile ci-core   # 9 binaries
bash scripts/fetch_decompilebench_corpus.sh --profile full      # 216 binaries
```

Staged under `tests/decompilebench/corpus/` (symlinks to algorithm-recovery
binaries). Stock Docker runs copy the real ELF files (dereferenced) into
`build/stock-docker-work/`.

## Usage

```bash
bash scripts/run_benchmarks.sh --profile ci-core --compare 2026-08
bash scripts/run_benchmarks.sh --profile full --compare 2026-08
```

Direct runner (`retdec-decompiler` from a CMake preset tree):

```bash
python3 tests/decompilebench/runner.py \
  --decompiler build/linux/src/retdec-decompiler/retdec-decompiler \
  --corpus tests/decompilebench/corpus \
  --out results/decompilebench.json
```

On Windows MSVC the binary is under `build/windows/`. On macOS the same
`full-linux-*` presets use `build/linux/`.

## Metrics

| Metric | Meaning |
|--------|---------|
| `syntax_valid` | Decompiler emitted non-empty `.c` |
| `recompile_success` | Decompiled C compiles with gcc |
| `coverage_equivalence` | Original vs recompiled binary: same exit code + stdout |
| `wall_s` | Decompile wall time |
| `peak_rss_kb` | Peak child RSS (Linux) |

`--buildable` is on by default in the product CLI; keep it on for recompile
figures. Do not mix Debug/WSL fork times with stock Release-in-Docker.

## Stock RetDec 5.0 comparison

Official Hub image `retdec/retdec:v5.0` does **not** exist. We pull
`remnux/retdec` (stock v5.0). Run from **Windows PowerShell** (WSL cannot exec
`docker.exe` until Docker Desktop WSL integration is restarted):

```powershell
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;" + $env:PATH
py -3 scripts\run_stock_retdec_docker.py --profile ci-core --skip-pull
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

Results: `results/stock-retdec-docker-ci-core.json`,
`results/stock-retdec-docker-full.json`
(see [`results/README.md`](../../results/README.md)). Local leftovers:
`data/archive/`. Stock has no algorithm-label export — F1 stays fork-only.

See [docs/internal/MAINTAINER_SCOPE.md](../../docs/internal/MAINTAINER_SCOPE.md).
