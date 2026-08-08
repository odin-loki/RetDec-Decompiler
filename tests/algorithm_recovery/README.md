# Algorithm recovery benchmark

Corpus: binaries built from sources with known algorithmic content (sorts,
hash tables, ring buffers, mutex patterns, serialisation). Ground truth: JSON
labels from source sidecars, never from the decompiler.

## Build corpus

```bash
bash scripts/build_algorithm_corpus.sh
```

Produces `tests/algorithm_recovery/corpus/` (6 sources × 3 opts × gcc/clang).

## Ground truth

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
