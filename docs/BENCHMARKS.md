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

Name-blind extract (`--no-stem-fallback`) is the headline figure.
ci-core `mean_f1` ≈ **0.335**. Full 216 name-blind mean **0.124**
(O0 0.137 / O2 0.116 / O3 0.121). The checked-in
`algorithm-recovery-full.json` `mean_f1` **1.0** is the withdrawn
stem-tuned score — do not advertise it.
Per-opt name-blind numbers: `results/algorithm-recovery-per-opt.md`.
Stock RetDec has no label export — F1 is fork-only.

B9 adversarial-positive (18 gcc O0/O2 binaries): name-blind mean
**0.093** (`results/b9-adversarial-positive.md`). Not a product F1.
Sentinel heapsort O2 now also extracts HashTable/Map after recovered
SSA carries immediates.

B8 loop-containing negatives (100): FP rate **0.000** after
the sort, RingBuffer, and copy-state-machine gates
(`results/b8-loop-negatives.md`). Loop-free B8 remains 0.000.

B10 zlib 1.3.1 crc32-only: 2/2 decompiled, name-blind F1 **0.000**.
crc+deflate still timed out (`results/b10-third-party.md`).

A4: 160 detections on remasured loop-negatives (`std::transform`,
`unordered_map`, `std::find_if`); extract still yields FP **0.000**.
Empirical precision **0**. Not fitted (`results/a4-calibration.md`).

Official full-corpus gate is still `MIN_MEAN_F1=0.95`; honest
name-blind is 0.124 (`results/algorithm-recovery-gate-finding.md`).

Q4 goto baseline on ci-core gcc O0/O2/O3: mean **1.44**
(`results/goto-optimizer-baseline.md`). O0 is still 0.

## Migration evals (optional)

`bash scripts/migration_eval_suite.sh` — rellic, LIEF, Retypd, SAILR scaffolds.
Outputs go to `results/` (gitignored) or `data/archive/evals/` if you archive
them. Not required to ship. See [internal/MAINTAINER_SCOPE.md](internal/MAINTAINER_SCOPE.md).
