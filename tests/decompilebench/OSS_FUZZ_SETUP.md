# OSS-Fuzz DecompileBench — not used in this fork

The paper corpus (arXiv 2505.11340, ~23k OSS-Fuzz functions) requires Docker,
oss-fuzz checkout, and patched base images. **This project does not use Docker
and does not pursue that corpus.**

## What we use instead

The **algorithm-recovery stand-in** (216 ELF binaries, GCC/Clang × O0/O2/O3):

```bash
bash scripts/fetch_decompilebench_corpus.sh --profile ci-core   # 9 binaries
bash scripts/fetch_decompilebench_corpus.sh --profile full      # 216 binaries
bash scripts/run_benchmarks.sh --profile full --compare 2026-08
```

That is sufficient for regression gates and documented baselines.

## Upstream reference only

`scripts/fetch_oss_fuzz_decompilebench.sh` clones upstream DecompileBench tooling
for reference. No further steps are expected on this machine.

See [docs/internal/MAINTAINER_SCOPE.md](../../docs/internal/MAINTAINER_SCOPE.md).
