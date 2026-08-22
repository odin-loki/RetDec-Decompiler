# DecompileBench fork vs stock

| Metric | Fork | Stock | Fork-buildable | Fork-refined |
|--------|------|-------|----------------|--------------|
| count | 9 | 9 | 9 | 0 |
| syntax_valid_rate | 1.000 | 1.000 | — | — |
| tu_valid_rate | 0.000 | — | 0.889 | — |
| recompile_success_rate | 0.000 | 0.000 | 0.889 | — |
| mean_wall_s | 1.836 | 0.249 | — | — |
| p50_wall_s | 1.579 | 0.250 | — | — |
| p90_wall_s | 1.834 | 0.264 | — | — |
| p99_wall_s | 1.834 | 0.264 | — | — |
| max_wall_s | 4.143 | 0.271 | — | — |

Wall times are decompiler process time. Sidecar columns score `.buildable.c` / `.refined.c` when present; they are not separate decompile runs.
fork/stock mean_wall_s ratio: 7.373

Notes (do not advertise a stock speed win):

- Stock is `remnux/retdec` v5.0 **Release** Docker. Docker was down; stock
  `tu_valid` was not re-measured. Stock `recompile_success` is 0.0.
- Fork is this tree's **Debug** `retdec-decompiler` on WSL (warm cache).
  Mean 1.836 s vs stock 0.249 s (**7.4×**). `pipeline.pm_run` dominates.
- Quality: raw `.c` `tu_valid` 0%. Opt-in `RETDEC_EMIT_BUILDABLE`
  `tu_valid_buildable` **0.889** (8/9) and `recompile_buildable` **0.889**
  (8/9). Recompile is `cc -o` on the single `.buildable.c` TU (cloned
  prototypes + weak helpers + `main`). Default output `.c` is unchanged.
- Algorithm recovery, name-blind extract: `mean_f1` ≈ **0.335**
  (`results/algorithm-recovery-ci.json`). Prior 1.0 figures stay withdrawn.
- E1: named vs hashed labels matched (`results/e1-real-binary-smoke.json`).
  Labels on `binary_search-gcc-O0` include `quicksort` / `std::partition` /
  `std::shared_ptr<T>` — not a quality claim.
- Neural: no GGUF. Mock path still produces a syntax-valid TU.
