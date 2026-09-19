# data/

Local and archived measurement artifacts for RetDec Imortek. **CI and
release scripts read [`results/`](../results/README.md), not this tree.**
v2.0.22 product numbers live in `results/` and are summarised in
[README.md](../README.md) / [docs/CLAIMS.md](../docs/CLAIMS.md).

This directory is for *your* working copies: per-commit dumps, eval JSON,
logs. It is not an installer payload and not a second baseline store.

| Path | What | Git |
|------|------|-----|
| [`../results/`](../results/README.md) | Live baselines + current stock/F1 JSON | committed |
| `archive/runs/` | Per-commit `results/<sha>.json` dumps | ignored |
| `archive/evals/` | Optional migration-eval JSON (rellic / LIEF / …) | ignored |
| `archive/tmp/` | `*-tmp.json` harness leftovers | ignored |
| `archive/logs/` | Local build / upload logs | ignored |
| `archive/legal/` | Generated license concatenations | ignored |
| `archive/vendor/` | Downloaded third-party zips | ignored |
| `archive/profile_run/` | Old decompile/profile working tree | ignored |

Do not commit files under `archive/` except this README. Regeneration
(from the repo root; these scripts exist):

```powershell
bash scripts/run_benchmarks.sh --profile ci-core --compare 2026-08
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

Stock compare uses `remnux/retdec` (official `retdec/retdec:v5.0` does not
exist). This fork does not use the OSS-Fuzz DecompileBench paper corpus.
