# Algorithm recovery benchmark

Corpus and harness for RetDec Imortek **specification extraction**.
Binaries are built from sources with known algorithmic content (sorts,
hash tables, ring buffers, mutex patterns, serialisation). Ground truth:
JSON labels from source sidecars, never from the decompiler.

v2.0.22 ships the detectors (`src/algo_recover`, `src/sort_detect`,
`src/container_detect`, `src/concurrency_detect`, …). This directory
**measures** them. Headline is **name-blind** F1: ci-core ~0.126, full 216
**0.056**. Do not advertise 1.0. See [METRIC.md](METRIC.md) and
[docs/CLAIMS.md](../../docs/CLAIMS.md) `C-ALGO-F1`.

## Build corpus (216+ binaries)

```bash
bash scripts/build_algorithm_corpus.sh
```

Generates catalog sources under `sources/generated/` plus 6 hand-written
sources. With gcc **and** clang: 36 × 3 opts × 2 compilers = **216 binaries**.

## Extract predictions from decompiler

Use a binary from a CMake preset (`full-linux-debug` → `build/linux`,
`full-windows-debug` → `build/windows`):

```bash
python3 scripts/extract_decompiler_predictions.py \
  --decompiler build/linux/bin/retdec-decompiler \
  --corpus tests/algorithm_recovery/corpus \
  --out tests/algorithm_recovery/predictions/corpus.json
```

## Sources

Hand-written (6): `bubblesort.c`, `mergesort.c`, `hash_table.c`,
`ring_buffer.c`, `binary_search.c`, `memcpy_loop.c`.

Generated catalog: `sources/generated/` via `scripts/generate_corpus_sources.py`
(quicksort, heapsort, insertion/selection/shell sort, graph DFS/BFS, knapsack,
LCS, pthread mutex, atomics, and more).

Knapsack, LCS, and Fibonacci remain **corpus labels only** (audit A6). No
structural detector assigns those kinds.

Other trees:

| Path | Role |
|------|------|
| `sources/negative/` | B8 non-algorithm programs — see [sources/negative/README.md](sources/negative/README.md) |
| `sources/negative_loops/` | Loop-containing negatives (`scripts/build_negative_loop_corpus.sh`) |
| `sources/adversarial/` | B9 idiosyncratic set |
| `sources/third_party/` | B10 (zlib) |

```bash
python3 scripts/generate_ground_truth.py \
  --sources tests/algorithm_recovery/sources \
  --manifest tests/algorithm_recovery/corpus/manifest.json \
  --out tests/algorithm_recovery/ground_truth/corpus.json
```

## Metrics

- precision, recall, F1 per detection class
- per optimisation level (-O0, -O2, -O3)
- per compiler (GCC, Clang)
- always report `mean_f1_raw` (no stem fallback)

```bash
python3 tests/algorithm_recovery/runner.py \
  --predictions tests/algorithm_recovery/predictions/sample.json \
  --ground-truth tests/algorithm_recovery/ground_truth/corpus.json
```

Run full harness via `bash scripts/run_benchmarks.sh --build-corpus`.

## CI (name-blind)

`ctest-linux` runs a 9-binary core subset after building `retdec-decompiler`:

```bash
bash scripts/run_algorithm_recovery_ci.sh --decompiler build/linux/bin/retdec-decompiler
```

Gate: `scripts/algorithm_recovery_gate.sh` requires ≥ 6 successful decompiles.
CI-core `MIN_MEAN_F1=0.12`; full-corpus `MIN_MEAN_F1=0.05`. Those are
regression floors, not product quality.

## Full corpus

```bash
bash scripts/run_algorithm_recovery_full.sh --decompiler build/linux/bin/retdec-decompiler --jobs 4
```

Nightly: `.github/workflows/algorithm-recovery-nightly.yml` (weekly CI core;
dispatch for full 216).

## Hand-written starter labels

| Source | Labels |
|--------|--------|
| bubblesort.c | BubbleSort, Sort |
| mergesort.c | Mergesort, DivideAndConquer |
| hash_table.c | HashTable, OpenAddressing |
| ring_buffer.c | RingBuffer |
| binary_search.c | BinarySearch |
| memcpy_loop.c | Memcpy, Copy |

Expand by adding sources under `sources/` with matching `.labels.json`.
