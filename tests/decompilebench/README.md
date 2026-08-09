# DecompileBench harness (arXiv 2505.11340)

Measures pseudocode quality: syntax validity, recompile success, coverage equivalence,
wall time, and peak RSS.

## Corpus (this fork)

We use the **algorithm-recovery stand-in** only — not the paper's OSS-Fuzz corpus
(Docker required upstream; **out of scope** here).

```bash
bash scripts/fetch_decompilebench_corpus.sh --profile ci-core   # 9 binaries
bash scripts/fetch_decompilebench_corpus.sh --profile full      # 216 binaries
```

Staged under `tests/decompilebench/corpus/` (symlinks to algorithm-recovery binaries).

## Usage

```bash
bash scripts/run_benchmarks.sh --profile ci-core --compare 2026-08
bash scripts/run_benchmarks.sh --profile full --compare 2026-08
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

**Not maintained.** Fork-vs-stock two-column tables leave stock as `—`.
`run_stock_retdec_docker.sh` and `fetch_stock_retdec.sh` exist for reference only.

See [docs/internal/MAINTAINER_SCOPE.md](../../docs/internal/MAINTAINER_SCOPE.md).
