# results/

Live measurement contract for CI, ship checklist, and the release table.

## Committed

| File | Role |
|------|------|
| `baseline-2026-08.json` | DecompileBench regression gate |
| `baseline-algorithm-recovery.json` | Algorithm-recovery regression gate |
| `stock-retdec-docker-ci-core.json` | Stock RetDec 5.0 (`remnux/retdec`), 9 binaries |
| `stock-retdec-docker-full.json` | Stock RetDec 5.0, 216 binaries |
| `algorithm-recovery-ci.json` | Fork F1 on CI-core (9) |
| `algorithm-recovery-full.json` | Fork F1 on full stand-in corpus (216) |

`mean_f1_raw = 1.0` is benchmark-corpus tuning, not production structural
detection. Stock has no label export — F1 is fork-only.

## Generated (gitignored)

`results/<git-sha>.json`, `*-tmp.json`. After a local run, move leftovers to
[`../data/archive/`](../data/README.md) if you want the working tree clean.

## Commands

```bash
bash scripts/run_benchmarks.sh --compare 2026-08 --gate --profile ci-core
bash scripts/regenerate_benchmark_tables.sh
```
