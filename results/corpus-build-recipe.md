# Corpus build recipe (B16)

No pinned compiler container digest is published. Docker is not available
in this WSL environment (`docker` is not installed). Host compilers are
recorded at build time.

## Stand-in algorithm-recovery corpus (216)

- Script: `scripts/build_algorithm_corpus.sh`
- Sources: `tests/algorithm_recovery/sources/*.c` and `sources/generated/`
- Skipped: `sources/negative/` (B8) and `sources/adversarial/` (B9)
- Compilers: host `gcc` and `clang` if present
- Optimisation: `-O0 -O2 -O3`
- Ground truth: gitignored `tests/algorithm_recovery/ground_truth/corpus.json`

## B8 negative corpus

- Script: `scripts/build_negative_corpus.sh`
- gcc `-O0` only
- Binaries gitignored under `tests/algorithm_recovery/negative_corpus/`

## B9 adversarial-positive corpus

- Script: `scripts/build_adversarial_corpus.sh`
- gcc `-O0` and `-O2`; `aes_ni.c` adds `-maes -msse2`
- `cc_version` is stored on each manifest row
- Binaries gitignored under `tests/algorithm_recovery/adversarial_corpus/`

## B11 holdout

Source SHA-256s: `tests/algorithm_recovery/holdout/source-hashes.json`.
This is the B9 source set, not a third-party Debian holdout.
