# DecompileBench harness (arXiv 2505.11340)

Placeholder runner for recompilation success and coverage equivalence benchmarks.

## Usage

```bash
bash scripts/run_benchmarks.sh --compare baseline-2026-08
```

## Planned layout

```
tests/decompilebench/
  runner.py          # invoke retdec-decompiler on corpus
  schema.json        # results/<git-sha>.json schema
  corpus/            # fetched separately (not committed)
```

## Metrics

- Syntax validity
- Recompile success (`-O0`, `-O2`, `-O3`)
- Coverage equivalence vs original binary
- Wall time and peak RSS

Run stock RetDec 5.0 through the same harness for two-column comparison.
