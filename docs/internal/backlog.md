# Internal backlog

| ID | Task | Executor | State |
|----|------|----------|-------|
| DB-1 | DecompileBench CI-core harness + coverage metric | C+ | **done** (stand-in corpus) |
| DB-2 | OSS-Fuzz full DecompileBench corpus | C+/H | `fetch_oss_fuzz_decompilebench.sh` scaffold |
| DB-3 | Stock RetDec 5.0 two-column table | C | `run_stock_retdec_docker.sh` / `RETDEC_STOCK_DECOMPILER` |
| AR-1 | `mean_f1_raw` vs `mean_f1` reporting | C | **done** |
| AR-2 | Per-opt-level F1 breakdown | C | **done** |
| AR-3 | Improve detector-only raw F1 (currently ~0.29 full) | C+ | open |
| LIEF-1 | `install_lief_sdk.sh` for Ubuntu 24.04 | C | **done** |
| LIEF-2 | FormatFactory cutover | C+ | open |
| OPS-1 | `gh auth` + nightly dispatch | H | blocked |
| OPS-2 | retdec-support regen | H | blocked |

See [PLAN_COMPLETION.md](PLAN_COMPLETION.md) and [NEXT_STEPS.md](NEXT_STEPS.md).
