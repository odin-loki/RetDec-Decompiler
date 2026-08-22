# DecompileBench fork vs stock

| Metric | Fork | Stock | Fork-buildable | Fork-refined |
|--------|------|-------|----------------|--------------|
| count | 9 | 9 | 9 | 0 |
| syntax_valid_rate | 1.000 | 1.000 | — | — |
| tu_valid_rate | 0.000 | — | 0.333 | — |
| recompile_success_rate | 0.000 | 0.000 | 0.000 | — |
| mean_wall_s | 1.393 | 0.249 | — | — |
| p50_wall_s | 1.379 | 0.250 | — | — |
| p90_wall_s | 1.544 | 0.264 | — | — |
| p99_wall_s | 1.544 | 0.264 | — | — |
| max_wall_s | 1.584 | 0.271 | — | — |

Wall times are decompiler process time. Sidecar columns score `.buildable.c` / `.refined.c` when present; they are not separate decompile runs.
fork/stock mean_wall_s ratio: 5.594

Notes (do not advertise a stock speed win from this row):

- Stock is `remnux/retdec` v5.0 **Release** Docker (`stock-retdec-docker-ci-core.json`).
  Docker was down; stock `tu_valid` was not re-measured.
- Fork is this tree's **Debug** `retdec-decompiler` on WSL, **warm** incremental
  cache: mean 1.393 s (p99 1.544 s) vs stock 0.249 s (**5.6×**). A cold Debug
  run on the same machine was 8.772 s (35×). Historical pre-B1 fork mean was
  1.492 s — warm Debug is in that band, not a 6× regression.
- `pipeline.pm_run` dominates. `analysis.detectors` is ~55 ms warm / ~4% cold.
  `RETDEC_SKIP_SEMANTIC_RECOVERY` drops detectors to ~3 ms and does not close
  the stock gap. See `docs/internal/DETECTOR_STAGE_COST.md`.
- Quality: raw `.c` `tu_valid` 0%. Opt-in `RETDEC_EMIT_BUILDABLE` sidecar
  `tu_valid_buildable` **0.333** (3/9). Stock and fork `recompile_success` 0%.
- Neural: no GGUF in-tree. Mock path
  `RETDEC_NEURAL_FORCE_MOCK` + `RETDEC_NEURAL_MOCK_EMIT_C` +
  `RETDEC_NEURAL_REQUIRE_COMPILE` produced a `cc -fsyntax-only` TU
  (`NEURAL_REFINED_TU_VALID=1`). Decompiled C is never executed.

