# Internal backlog

| ID | Task | State | Notes |
|----|------|-------|-------|
| DB-1 | DecompileBench CI-core harness + coverage metric | **done** | Stand-in corpus (216 max) |
| DB-2 | OSS-Fuzz full DecompileBench corpus | **out of scope** | Paper corpus / oss-fuzz farm |
| DB-3 | Stock RetDec 5.0 two-column table | **done** | `remnux/retdec` (v5.0); `retdec/retdec:v5.0` does not exist |
| AR-1 | `mean_f1_raw` vs `mean_f1` reporting | **done** | |
| AR-2 | Per-opt-level F1 breakdown | **done** | |
| AR-3 | Detector raw F1 on stand-in corpus | **done** | v2.0.19; benchmark-tuned caveat |
| LIEF-1 | `install_lief_sdk.sh` | **done** | |
| LIEF-2 | FormatFactory cutover | **optional** | Adapter works |
| OPS-1 | `gh auth` + nightly dispatch | **optional** | Windows `gh` only |
| OPS-2 | retdec-support regen | **out of scope** | Upstream tarball sufficient |

See [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md), [PLAN_COMPLETION.md](PLAN_COMPLETION.md).
