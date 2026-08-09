#!/usr/bin/env bash
# fetch_oss_fuzz_decompilebench.sh — REFERENCE ONLY (OSS-Fuzz corpus not used; no Docker).
# See docs/internal/MAINTAINER_SCOPE.md and tests/decompilebench/OSS_FUZZ_SETUP.md
#
# Usage: bash scripts/fetch_oss_fuzz_decompilebench.sh [--clone-only]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/external/oss-fuzz-decompilebench"
CLONE_ONLY=false

while [[ $# -gt 0 ]]; do
	case "$1" in
		--clone-only) CLONE_ONLY=true; shift ;;
		*) echo "Unknown arg: $1" >&2; exit 1 ;;
	esac
done

mkdir -p "${ROOT}/external"

if [[ ! -d "${DEST}/.git" ]]; then
	echo "==> Cloning DecompileBench tooling"
	git clone --depth 1 https://github.com/Jennieett/DecompileBench.git "${DEST}"
else
	echo "DecompileBench repo already at ${DEST}"
fi

cat > "${ROOT}/tests/decompilebench/OSS_FUZZ_SETUP.md" <<'MD'
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
MD

echo "Scaffold ready: ${DEST}"
echo "See tests/decompilebench/OSS_FUZZ_SETUP.md for manual steps."

if [[ "${CLONE_ONLY}" == true ]]; then
	exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
	echo "docker not available — stopped after clone (use --clone-only)" >&2
	exit 2
fi

echo "Docker found. Complete image build per OSS_FUZZ_SETUP.md (human-led)."
