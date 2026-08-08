# RetDec GUI Polish Checklist

Polish-only work (no new features). Update as items ship.

## Completed (v3 maintenance)

Framework / docs polish shipped alongside GUI v3 (May 2026). Items below were on the maintenance backlog; all complete.

- [x] **CI badges** — README workflow badges for `perf-nightly` and `release-installers` only (`ctest-windows` / `ctest-linux` are manual-only, no badge)
- [x] **Unit tests in CI** — GUI headless tests + `ctest -L unit` in `.github/workflows/ctest-windows.yml` and `ctest-linux.yml`
- [x] **`retdec_cli.py` autodetect** — `_resolve_decompiler()` searches `install/`, `build/**`, and `build-decompiler-test/bin`
- [x] **User manual sync** — `docs/user_manual.md` aligned with v3 layout; external `retdec-qwen3-runner` / CLI `--model` (no in-GUI AI chat)
- [x] **`scripts/README.md` expansion** — CI/tooling table (`retdec_cli.py`, unpack/decompile, perf bench, install smoke, build-all)

## Tier 0 — Trust (stop lying) ✅
- [x] Hide/wire IR stage combo, Assembly G/F, call graph depth, tri-pane Alt+←/→
- [x] Settings: theme/font honest, ML copy fixed, analysis scope banner
- [x] restoreSession disabled with tooltip; language label honest

## Tier 1 — Navigation coherence ✅
- [x] CFG / call graph / type hierarchy / strings / diagnostics → function list + tabs
- [x] F5 and Run Stage continue after open-binary dialog
- [x] Synced tab loads asm/IR/C per function
- [x] Decompiled C scroll-to-function; Assembly scroll-to-address

## Tier 2 — Surface hidden panels ✅
- [x] Binary Browser tab in workspace dock
- [x] Command log as History tab in output dock
- [x] Progress tab (auto-shown during decompile)

## Tier 3 — Visual / empty states ✅
- [x] EmptyStateWidget; per-panel empty states
- [x] QSS: spinbox, list, toolbutton, dialog button box, tree expanders
- [x] Status bar: — functions default, progress percent visible
- [x] Diagnostics severity icons; diff empty colour fixed

## Tier 4 — Shortcuts & copy ✅
- [x] Ctrl+S / Ctrl+Shift+S split; Ctrl+, Settings; View → Reset layout
- [x] Help → Keyboard Shortcuts dialog; About updated

## Tier 5 — Type hierarchy & CFG content ✅
- [x] classes/vtables from config.json
- [x] CFG block instructions from DSM lines
- [x] Type hierarchy empty state

## Tier 6 — Tests ✅
- [x] polish_integration_test.cpp; layout tests for 3+4 tabs

## Tier 7 — Polish v2 ✅
- [x] Target panel in workspace dock (4th tab) + Analysis → Target + clickable triage badges
- [x] Light theme (`catppuccin_latte.qss`) + Settings theme combo enabled
- [x] Inline panel styles → QSS role properties (triage, function list, call graph, tri-pane, console)
- [x] Decompiled C find bar (Ctrl+F, F3, Shift+F3, Esc)
- [x] Compare as non-modal tool window + `lastCompareDir` QSettings memory
- [x] Diff panel current-line highlight
- [x] Empty start screen (Open Binary / Project / drag-drop hint)
- [x] Restore last session wired (`lastBinaryPath`, skipped in headless)
- [x] Status bar decompile state (`not decompiled` / `cached` / `decompiled`)
- [x] Document tab icons; dead ML Assistant stub removed

## Remaining (optional)
- [ ] i18n
- [x] Unified context menus (Copy Address, Go to Function) on Functions, Assembly, CFG
- [x] Loading status on artifact load (`Loading artifacts…` in status bar)
- [x] Session persistence beyond layout (function-list filter text, call-graph depth per `.retdec` project)
- [x] Tri-pane language combo: sync from Settings on load; tooltip + hint on manual change (view-only syntax)
- [x] Optional Linux AppImage in `release-installers.yml` (`APPIMAGE=1` or workflow_dispatch checkbox)
- [x] `ci-smoke.yml`: run `scripts/check_format.sh` when present (5 min timeout)
- [x] README links to CONTRIBUTING.md / SECURITY.md; mentions `scripts/doctor.ps1`
