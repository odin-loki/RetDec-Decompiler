# Algorithm recovery benchmark scaffold

Corpus: binaries built from sources with known algorithmic content (sorts,
hash tables, mutex patterns, serialisation). Ground truth: JSON labels from
source, never from the decompiler.

## Layout (planned)

```
tests/algorithm_recovery/
  corpus/           # built binaries (not committed)
  ground_truth/     # *.json labels per binary
  runner.py         # precision/recall/F1 per class
```

## Metrics

- precision, recall, F1 per detection class
- per optimisation level (-O0, -O2, -O3)
- per compiler (GCC, Clang)

Run via `scripts/run_benchmarks.sh`.

## Sample data

```bash
python3 tests/algorithm_recovery/runner.py \
  --predictions tests/algorithm_recovery/predictions/sample.json \
  --ground-truth tests/algorithm_recovery/ground_truth/sample.json
```
