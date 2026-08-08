# Next steps (human-led)

MASTER-UPGRADE-PLAN automation is **complete** through v2.0.0. These items
require your environment, toolchain farm, or measured CI artifacts.

## 1. Raise algorithm-recovery F1 floor

**Done locally (2026-08-08, v2.0.3):** CI core baseline `mean_f1≈0.79`, `decompiled=9`; full corpus `216/216`, `mean_f1≈0.17`.
Re-run after detector tuning:

```bash
bash scripts/run_algorithm_recovery_ci.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)"
bash scripts/update_algorithm_recovery_baseline.sh \
  --from results/algorithm-recovery-ci.json \
  --profile ci_core
```

Then tighten `run_algorithm_recovery_ci.sh` `--min-mean-f1` if desired.

## 2. retdec-support regeneration

On a machine with MSVC, GCC, Clang, and MinGW runtimes:

```bash
bash scripts/regenerate-retdec-support.sh
# Follow build/support-regen/README.txt
# Upload tarball; update cmake/deps.cmake SUPPORT_PKG_URL + SHA256
```

## 3. Full-corpus nightly (216 binaries)

GitHub Actions → **algorithm-recovery-nightly** → Run workflow →
`full_corpus: true`. Review `results/algorithm-recovery-full.json` artifact.

## 4. Library migrations (months each)

| Step | Script | Blocker |
|------|--------|---------|
| 28 rellic | `eval_rellic.sh` | Build rellic on LLVM 8 |
| 29 LIEF | `eval_lief.sh` | `RETDEC_ENABLE_LIEF=ON` integration |
| 30 Retypd | `eval_retypd.sh` | LLVM module export path |
| 31 SAILR | `eval_sailr.sh` | Structure recovery backend |
| 33 LLVM | `inventory_llvm_apis.sh` | Retypd-first per plan |

## 5. Performance

Use weekly `perf-nightly` artifacts and `scripts/flamegraph_profile.sh` on
target hardware to pick hot paths before code changes.

## 7. Local Linux F1 (WSL)

```bash
# In WSL, from repo root:
bash scripts/wsl_build_decompiler.sh
bash scripts/run_algorithm_recovery_ci.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)"
```

First build takes 30–90 minutes. Prefer CI artifacts from `algorithm-recovery-nightly` when available.
