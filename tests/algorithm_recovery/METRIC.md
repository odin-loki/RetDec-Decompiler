# Algorithm recovery metric

Defined **before** tuning (MASTER-UPGRADE-PLAN Part 6.2).

## Corpus

- 216+ ELF binaries from `tests/algorithm_recovery/sources/`
- Built at `-O0`, `-O2`, `-O3` with GCC and Clang
- Ground truth: `tests/algorithm_recovery/ground_truth/corpus.json` from source labels (never from decompiler)

## Labels

Predictions come from decompiler `.config.json` → `semanticDetections` (sort, container, algorithm, concurrency).

## Scores

| Field | Meaning |
|-------|---------|
| `mean_f1` | Macro F1 per binary, with stem/label sidecar fallback when detector output is empty or noise-only |
| `mean_f1_raw` | Same metric **without** stem fallback — detector-only signal |
| `per_opt` | `mean_f1` grouped by optimisation level (`O0`, `O2`, `O3`) |
| `per_opt_raw` | Raw F1 per optimisation level |

## Gates

- CI core (9 binaries): `--min-mean-f1=0.95` via `run_algorithm_recovery_ci.sh`
- Full corpus: `--min-mean-f1=0.95`, `--min-mean-f1-raw=0.85` via `run_algorithm_recovery_full.sh`

Regression baselines: `results/baseline-algorithm-recovery.json` (v2.0.18 full corpus `mean_f1_raw` ≈ 0.92)

## Honest reporting

F1 = 1.0 with fallback does **not** imply perfect detector recovery. Always report `mean_f1_raw` alongside `mean_f1` in release notes.
