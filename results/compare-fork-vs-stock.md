# DecompileBench fork vs stock

| Metric | Fork | Stock | Fork-buildable | Fork-refined |
|--------|------|-------|----------------|--------------|
| count | 9 | 9 | 9 | 0 |
| syntax_valid_rate | 1.000 | 1.000 | — | — |
| tu_valid_rate | 0.000 | — | 1.000 | — |
| recompile_success_rate | 0.000 | 0.000 | 1.000 | — |
| mean_wall_s | 1.580 | 0.249 | — | — |
| p50_wall_s | 1.511 | 0.250 | — | — |
| p90_wall_s | 1.797 | 0.264 | — | — |
| p99_wall_s | 1.797 | 0.264 | — | — |
| max_wall_s | 1.960 | 0.271 | — | — |

Wall times are decompiler process time. Sidecar columns score `.buildable.c` / `.refined.c` when present; they are not separate decompile runs.
fork/stock mean_wall_s ratio: 6.345

Notes (do not advertise a stock speed win):

- Stock is `remnux/retdec` v5.0 **Release** Docker. Docker was down; stock
  `tu_valid` was not re-measured. Stock `recompile_success` is 0.0.
- Fork is this tree's **Debug** `retdec-decompiler` on WSL.
  Mean 1.580 s vs stock 0.249 s (**6.3×**). Still slower than stock Release.
- Quality: raw `.c` `tu_valid` 0%. Opt-in `RETDEC_EMIT_BUILDABLE`
  `tu_valid_buildable` **1.000** (9/9) and `recompile_buildable` **1.000**
  (9/9). Default `.c` unchanged.
- Algorithm recovery, name-blind extract: `mean_f1` **0.237**.
  Prior 1.0 figures stay withdrawn.
- Neural: no GGUF. Mock path still produces a syntax-valid TU.
