# DecompileBench fork vs stock

| Metric | Fork | Stock | Fork-buildable | Fork-refined |
|--------|------|-------|----------------|--------------|
| count | 216 | 216 | 216 | 0 |
| syntax_valid_rate | 1.000 | 1.000 | — | — |
| tu_valid_rate | 0.000 | — | 0.912 | — |
| recompile_success_rate | 0.000 | 0.000 | 0.912 | — |
| mean_wall_s | 1.716 | 0.242 | — | — |
| p50_wall_s | 1.333 | 0.230 | — | — |
| p90_wall_s | 2.143 | 0.284 | — | — |
| p99_wall_s | 5.864 | 0.432 | — | — |
| max_wall_s | 8.898 | 0.619 | — | — |

Wall times are decompiler process time. Sidecar columns score `.buildable.c` / `.refined.c` when present; they are not separate decompile runs.
fork/stock mean_wall_s ratio: 7.091

Notes (do not advertise a stock speed win):

- Full stand-in corpus: 216 algorithm-recovery binaries (gcc/clang × O0/O2/O3).
- Stock is `remnux/retdec` v5.0 **Release** Docker JSON
  (`results/stock-retdec-docker-full.json`). Stock `recompile_success` 0.0.
  Stock `tu_valid` was not re-measured (Docker down).
- Fork is Debug `retdec-decompiler` on WSL. Mean 1.716 s vs stock 0.242 s
  (**7.1×**).
- Quality: raw `.c` `tu_valid` 0%. Opt-in buildable `tu_valid` **0.912**
  (197/216) and `recompile` **0.912**. Default `.c` unchanged.
- ci-core (9 binaries) is in `results/compare-fork-vs-stock.md` (9/9).
