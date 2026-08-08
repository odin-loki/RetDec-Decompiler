# Algorithm recovery benchmark

Corpus: binaries built from sources with known algorithmic content (sorts,
hash tables, ring buffers, mutex patterns, serialisation). Ground truth: JSON
labels from source sidecars, never from the decompiler.

## Build corpus (216+ binaries)

```bash
bash scripts/build_algorithm_corpus.sh
```

Generates 30 catalog sources under `sources/generated/` plus 6 hand-written sources.
With gcc **and** clang: 36 × 3 opts × 2 compilers = **216 binaries**.

## Extract predictions from decompiler

```bash
scripts/extract_decompiler_predictions.py \
  --decompiler build/linux/bin/retdec-decompiler \
  --corpus tests/algorithm_recovery/corpus \
  --out tests/algorithm_recovery/predictions/corpus.json
```

## Sources (v1.3.0)

Hand-written (6): bubblesort, mergesort, hash_table, ring_buffer, binary_search, memcpy_loop.

Generated catalog (30): quicksort, heapsort, insertion/selection/shell sort, graph DFS/BFS, knapsack, LCS, pthread mutex, atomics, and more — see `scripts/generate_corpus_sources.py`.

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

```bash
python3 tests/algorithm_recovery/runner.py \
  --predictions tests/algorithm_recovery/predictions/sample.json \
  --ground-truth tests/algorithm_recovery/ground_truth/corpus.json
```

Run full harness via `bash scripts/run_benchmarks.sh --build-corpus`.

## CI live F1 (v1.4.0)

`ctest-linux` runs a 9-binary core subset after building `retdec-decompiler`:

```bash
bash scripts/run_algorithm_recovery_ci.sh --decompiler build/linux/bin/retdec-decompiler
```

Gate: `algorithm_recovery_gate.sh` requires ≥ 6 successful decompiles.

## Sources (v1.2.0 starter set)

| Source | Labels |
|--------|--------|
| bubblesort.c | BubbleSort, Sort |
| mergesort.c | Mergesort, DivideAndConquer |
| hash_table.c | HashTable, OpenAddressing |
| ring_buffer.c | RingBuffer |
| binary_search.c | BinarySearch |
| memcpy_loop.c | Memcpy, Copy |

Expand to 200+ binaries by adding sources under `sources/` with matching `.labels.json`.
