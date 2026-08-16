# data/

Local and archived measurement artifacts. **CI and release scripts read
`results/`, not this tree.**

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

Do not commit files under `archive/` except this README. Regeneration:

```powershell
bash scripts/run_benchmarks.sh --profile ci-core --compare 2026-08
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```
