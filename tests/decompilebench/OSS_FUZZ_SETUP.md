# OSS-Fuzz DecompileBench setup (arXiv 2505.11340)

This is **not** automated end-to-end. Follow upstream:

1. Install Docker + enable WSL2 integration (Windows).
2. Clone oss-fuzz and apply patches from `external/oss-fuzz-decompilebench/`.
3. Load base Docker images from upstream docs.
4. Extract function corpus per DecompileBench README.
5. Point `scripts/run_benchmarks.sh` at `tests/decompilebench/corpus/oss-fuzz/`.

Until then, use the algorithm-recovery stand-in:

```bash
bash scripts/fetch_decompilebench_corpus.sh --profile ci-core
```
