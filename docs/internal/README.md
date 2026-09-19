# Internal docs

Maintainer notes for **v2.0.24**. Start with **[MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md)**.

Public product page: [../../README.md](../../README.md). Historical 18-part plan:
[MASTER-UPGRADE-PLAN.md](MASTER-UPGRADE-PLAN.md) — not the live execution list.

## Current (as of 2.0.24)

What shipped and what maintainers still need:

| Doc | Role |
|-----|------|
| [MAINTAINER_SCOPE.md](MAINTAINER_SCOPE.md) | What we ship vs skip |
| [PLAN_COMPLETION.md](PLAN_COMPLETION.md) | Automation steps 1–26 plus 2.0.24 release/CI |
| [NEXT_STEPS.md](NEXT_STEPS.md) | Optional follow-ups (not a second master plan) |
| [REPRODUCIBILITY.md](REPRODUCIBILITY.md) | What the decompiler's output may not depend on |
| [wire-sass.md](wire-sass.md) | NVIDIA SASS cubin/fatbin library; CLI/fileformat patches; **not Production** |
| [UNFIXED_AUDIT_FINDINGS.md](UNFIXED_AUDIT_FINDINGS.md) | Audit log. Default-`.c` recompile is **216/216** (not 0/216). Remaining real findings are listed at the top of that file |
| [backlog.md](backlog.md) | ID table (done / out of scope / optional) |
| [D7_DECISION.md](D7_DECISION.md) | Specification-extraction positioning (matches README) |
| [LLVM_MIGRATION_SCOPE.md](LLVM_MIGRATION_SCOPE.md) | Pin is llvm-project **23.1.0**. No bump unless explicitly tasked |
| [DETECTOR_STAGE_COST.md](DETECTOR_STAGE_COST.md) | C9 — how to measure detector-stage cost |
| [`.cursorrules`](../../.cursorrules) | Composer agent rules (keep at repo root; do not delete) |

## Historical / optional

Plans and spikes. Do not treat open checkboxes as current work.

| Doc | Role |
|-----|------|
| [MASTER-UPGRADE-PLAN.md](MASTER-UPGRADE-PLAN.md) | Original 18-part plan (complete) |
| [UNBLOCKED-MIGRATION.md](UNBLOCKED-MIGRATION.md) | Wave 5 LLVM-8→upstream plan; pin already 23.1.0 |
| [EXECUTION_PLAN.md](EXECUTION_PLAN.md) | 2026-08-22 audit/benchmark waves; superseded |
| [ENGINEERING_ROADMAP.md](ENGINEERING_ROADMAP.md) | Shippable engineering tiers (GUI v3 / Tiers 1–5 landed) |
| [GUI_ROADMAP.md](GUI_ROADMAP.md) / [GUI_PHASE_D.md](GUI_PHASE_D.md) / [GUI_POLISH.md](GUI_POLISH.md) / [retdec_gui_plan.md](retdec_gui_plan.md) | GUI v3 — shipped |
| [PIPELINE_REDESIGN_TODO.md](PIPELINE_REDESIGN_TODO.md) | 29-stage gap checklist (hook/partial still open) |
| [lief_adoption.md](lief_adoption.md) / [rellic_evaluation.md](rellic_evaluation.md) / [retypd_sailr_llvm.md](retypd_sailr_llvm.md) | Research spikes |
| [C_ABI_SKETCH.md](C_ABI_SKETCH.md) | P1 design only; not shipped |
| [E1_REAL_BINARY_TESTS.md](E1_REAL_BINARY_TESTS.md) / [GOTO_OPTIMIZER_BASELINE.md](GOTO_OPTIMIZER_BASELINE.md) | Measurement notes |
| [dual-license-setup.md](dual-license-setup.md) / [modified-license.md](modified-license.md) | License drafts; canonical text is `LICENSE*` at repo root |
| [nightly/](nightly/README.md) | Nightly report folder |

Published numbers: [../BENCHMARKS_TABLE.md](../BENCHMARKS_TABLE.md),
[`../../results/`](../../results/README.md).
