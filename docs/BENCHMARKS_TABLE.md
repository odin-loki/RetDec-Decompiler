# Benchmark tables

- **Version:** 2.0.22
- **Stock image:** `remnux/retdec` (RetDec v5.0, commit `53e55b4`, 2022-12-08)
- **Generated:** wall-time and DecompileBench quality from
  `results/compare-fork-vs-stock-full.md` / `results/decompilebench-full.json`
  (fork mean_wall **1.416** s). CC-01 quality rows cite ctest-linux run 276.

Official Hub image `retdec/retdec:v5.0` does not exist.

## DecompileBench (stand-in corpus, 216 binaries)

Headline quality metric is **buildable C** (`--buildable`, default on), not default `.c`.

| Metric | Fork | Stock RetDec 5.0 |
|--------|------|------------------|
| Recompile, `--buildable` sidecar | **216/216** | **0/216** |
| Recompile, default `.c` (CC-01 run 276) | **216/216** | 0/216 |
| Recompile, default `.c` (`results/compare-fork-vs-stock-full.md`) | **0/216** | 0/216 |
| syntax_valid_rate (default `.c`) | 1.0 | 1.0 |
| mean_wall_s | 1.416 | 0.242 |

`mean_wall_s` is **not a comparison**: fork is Debug on WSL; stock is
Release inside Docker (`results/compare-fork-vs-stock-full.md` ratio **5.9×**).
Treat that ratio as unmeasured until both sides are Release on the same
hardware and container. The previous table cell **1.492** did not match the
checked-in JSON and is not republished.

Public Results lead with CC-01 **216/216** for default `.c`. The compare
markdown still records DecompileBench `tu_valid` **0** on default `.c` —
cite the file, do not silently drop it.

**This fork's default-`.c` figure was `0/216` until run 274, and it was stale
when it was written.** It was taken before `NoInitVarDefOptimizer` stopped
being run for the C back end, which is what made the emitted C assign to
undeclared variables. CC-01 (`scripts/ci/check_emitted_c_compiles.sh`) hands
each emitted `.c` to a C compiler, over the whole corpus and over a 24-binary
slice, on every `ctest-linux` run. Run 274, at `42cdb06`, was the first
whole-corpus measurement and reported 208/216. Run 276, at `2c3669d`, reports
**216/216**. Both rates are floors now. The live job also names a **252**-file
corpus (six FP sources); that later count was not re-measured here.

The eight failures run 274 found were three defects, fixed across runs 275 and
276: a `break` hoisted out of the loop it belonged to (`hash_table`, four
builds), `pthread_create` called with one argument where `<pthread.h>` declares
four (`generated_pthread_mutex`, two builds), and a `void *` as an operand of
`|` (`generated_bloom_filter`, two builds). Name-blind F1 held at 0.2302 and
DET-01 at 72 binaries with 0 skipped throughout.

The per-optimisation-level default-`.c` recompile column is not republished
here. Name-blind F1 per opt is below. Stock's 0/216 on buildable C is
unaffected and not re-measured here.

Artifacts: `results/compare-fork-vs-stock-full.md`,
`results/stock-retdec-docker-full.json`.

## Algorithm recovery (name-blind is the headline)

| Profile | mean F1 (name-blind) | 95% CI | n |
|---------|----------------------|--------|---|
| Full corpus | **0.056** | 0.034 – 0.083 | 216 |
| CI core | 0.126 | — | 9 |

Name-assisted (symbolicated binaries, stem/label fallback) scores 1.0 on
this corpus because function names match the labels. That is a **second
mode**, not the headline. Do not advertise 1.0.

The withdrawn stem-tuned `mean_f1` column is not published here.

Per-opt name-blind: O0 0.050 / O2 0.059 / O3 0.059
(`results/algorithm-recovery-per-opt.md`).
Stock RetDec has no label export — F1 is fork-only.

Artifacts: `results/algorithm-recovery-full-nameblind.json`.
Temporary harness dumps belong in `data/archive/`.

_Regenerate fork: `python3 tests/decompilebench/runner.py ...`. Stock: `py -3 scripts/run_stock_retdec_docker.py --profile full --skip-pull`._
