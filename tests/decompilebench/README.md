# DecompileBench harness (arXiv 2505.11340)

Measures pseudocode quality: syntax validity, recompile success, coverage equivalence,
wall time, and peak RSS.

## Corpus

**Full paper corpus** (23,400 OSS-Fuzz functions) requires Docker + [Jennieett/DecompileBench](https://github.com/Jennieett/DecompileBench) setup — not yet wired in-tree.

**Local stand-in** (reproducible, with source for coverage checks):

```bash
bash scripts/fetch_decompilebench_corpus.sh --profile ci-core   # 9 binaries
bash scripts/fetch_decompilebench_corpus.sh --profile full      # 216 binaries
```

Staged under `tests/decompilebench/corpus/` (symlinks to algorithm-recovery binaries).

## Usage

```bash
bash scripts/run_benchmarks.sh --profile ci-core --compare 2026-08
bash scripts/run_benchmarks.sh --profile ci-core --fetch-stock   # two-column vs RetDec v5.0
```

Direct runner:

```bash
python3 tests/decompilebench/runner.py \
  --decompiler build/linux/src/retdec-decompiler/retdec-decompiler \
  --corpus tests/decompilebench/corpus \
  --out results/decompilebench.json
```

## Metrics

| Metric | Meaning |
|--------|---------|
| `syntax_valid` | Decompiler emitted non-empty `.c` |
| `recompile_success` | Decompiled C compiles with gcc |
| `coverage_equivalence` | Original vs recompiled binary: same exit code + stdout |
| `wall_s` | Decompile wall time |
| `peak_rss_kb` | Peak child RSS (Linux) |

## Stock RetDec 5.0 comparison

```bash
bash scripts/fetch_stock_retdec.sh
bash scripts/run_benchmarks.sh --profile ci-core --fetch-stock
```

Results include `stock_retdec` and `compare.fork_vs_stock` in `results/<sha>.json`.
