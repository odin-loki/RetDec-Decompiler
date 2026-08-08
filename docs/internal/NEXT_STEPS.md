# Next steps (human-led)

MASTER-UPGRADE-PLAN automation through v2.0.14+. These items
require your environment, toolchain farm, or measured CI artifacts.

## Local automation (no WSL required)

```bash
bash scripts/automation_status.sh       # what's done vs blocked
bash scripts/run_all_automation.sh      # eval venv, doctor, ship checklist, tests, migration evals
bash scripts/run_all_automation.sh --skip-migration   # faster smoke
bash scripts/setup_eval_venv.sh         # PEP 668-safe Python deps for migration evals
```

Windows: `.\scripts\doctor.ps1`, `.\scripts\run_all_automation.ps1`, and `.\scripts\dispatch_algorithm_recovery_nightly.ps1`

## 1. Algorithm-recovery F1 (local baselines)

**Done locally (2026-08-08, v2.0.7):**

| Profile | Decompiled | mean F1 |
|---------|------------|---------|
| CI core (9 binaries) | 9/9 | 1.0 |
| Full corpus (216 binaries) | 216/216 | 1.0 |

Re-run after detector tuning:

```bash
bash scripts/run_algorithm_recovery_ci.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)"
bash scripts/run_algorithm_recovery_full.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)" --jobs 4
bash scripts/update_algorithm_recovery_baseline.sh \
  --from results/algorithm-recovery-ci.json \
  --profile ci_core
bash scripts/update_algorithm_recovery_baseline.sh \
  --from results/algorithm-recovery-full.json \
  --profile full_corpus
```

Gates: CI `--min-mean-f1=0.95`; full corpus `--min-mean-f1=0.95`.

## 2. retdec-support regeneration

On a machine with MSVC, GCC, Clang, and MinGW runtimes:

```bash
bash scripts/regenerate-retdec-support.sh
# Follow build/support-regen/README.txt
# Upload tarball; update cmake/deps.cmake SUPPORT_PKG_URL + SHA256
```

## 3. Full-corpus nightly (216 binaries)

```bash
# Windows (GitHub CLI installed):
gh auth login
.\scripts\dispatch_algorithm_recovery_nightly.ps1
.\scripts\dispatch_algorithm_recovery_nightly.ps1 -FullCorpus

# WSL (uses Windows gh.exe when available):
bash scripts/find_gh.sh
bash scripts/dispatch_algorithm_recovery_nightly.sh --full-corpus
```

Or manually: `gh workflow run algorithm-recovery-nightly -f full_corpus=true`

Review `results/algorithm-recovery-full.json` artifact from the workflow run.

## 4. Library migrations (months each)

| Step | Script | Blocker |
|------|--------|---------|
| 28 rellic | `eval_rellic.sh` | Build rellic on LLVM 8 |
| 29 LIEF | `eval_lief.sh` | Python: `bash scripts/setup_eval_venv.sh`. C++: `bash scripts/install_lief_sdk.sh` (Ubuntu 24.04 has no `liblief-dev`) |
| 30 Retypd | `eval_retypd.sh` | LLVM module export path |
| 31 SAILR | `eval_sailr.sh` | Structure recovery backend |
| 33 LLVM | `inventory_llvm_apis.sh` | Retypd-first per plan |

Migration eval scaffolds run via `bash scripts/migration_eval_suite.sh --decompiler PATH`.

## 5. Performance

Use weekly `perf-nightly` artifacts and `scripts/flamegraph_profile.sh` on
target hardware to pick hot paths before code changes.

## 6. Local Linux F1 (WSL)

```bash
# In WSL, from repo root:
bash scripts/wsl_build_decompiler.sh
bash scripts/run_algorithm_recovery_ci.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)"
bash scripts/run_algorithm_recovery_full.sh --decompiler "$(find build/linux -name retdec-decompiler -type f | head -n1)" --jobs 4
```

First build takes 30–90 minutes. Prefer CI artifacts from `algorithm-recovery-nightly` when available.
