# Benchmarks

Live artifacts live in [`results/`](../results/README.md). Historical SHA
dumps, eval JSON, and local logs live in [`data/archive/`](../data/README.md)
(not committed).

Harness: `scripts/run_benchmarks.sh` writes `results/<git-sha>.json`.

Regression gates:

- DecompileBench: `scripts/benchmark_regression_gate.sh` vs `results/baseline-2026-08.json`
- Algorithm recovery: `scripts/algorithm_recovery_regression_gate.sh` vs `results/baseline-algorithm-recovery.json`

Release table: [`BENCHMARKS_TABLE.md`](BENCHMARKS_TABLE.md) via
`scripts/regenerate_benchmark_tables.sh`.

## DecompileBench (pseudocode quality)

Stand-in corpus (same 216 ELF binaries as algorithm recovery), **not** the
OSS-Fuzz 23k paper corpus. Measures syntax validity, recompile, and wall time.

Stock column is `remnux/retdec` (RetDec v5.0). Official Hub image
`retdec/retdec:v5.0` does not exist.

```powershell
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;" + $env:PATH
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

On this set both sides are typically syntax 1.0 and recompile 0% (RetDec
pseudocode is not gcc-clean C).

Harness: [tests/decompilebench/README.md](../tests/decompilebench/README.md)

## Algorithm recovery (product differentiator)

Custom metric for `algo_recover`, `sort_detect`, `concurrency_detect`,
`container_detect`, and `serial_detect`.

| Component | Path |
|-----------|------|
| Corpus (216 binaries) | `tests/algorithm_recovery/corpus/` (built locally) |
| Ground truth | `scripts/build_algorithm_corpus.sh` → gitignored `ground_truth/corpus.json` |
| CI core (9 binaries) | `scripts/run_algorithm_recovery_ci.sh` |
| Full corpus | `scripts/run_algorithm_recovery_full.sh --jobs 4` |
| Nightly | `.github/workflows/algorithm-recovery-nightly.yml` |

`mean_f1` uses stem/label fallback. `mean_f1_raw` is detector-only.
**`mean_f1_raw = 1.0` is corpus-tuned** (stem augment + extract noise strip),
not proof that structural IR detection is solved in production. Stock RetDec
has no label export — F1 is fork-only.

## Migration evals (optional)

`bash scripts/migration_eval_suite.sh` — rellic, LIEF, Retypd, SAILR scaffolds.
Outputs go to `results/` (gitignored) or `data/archive/evals/` if you archive
them. Not required to ship. See [internal/MAINTAINER_SCOPE.md](internal/MAINTAINER_SCOPE.md).
