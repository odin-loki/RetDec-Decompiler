# RetDec GUI Polish Checklist

Polish-only work (no new features). Update as items ship.

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
- [ ] Unified context menus (Copy address, Go to function) across all panels
- [ ] Loading spinners on large artifact loads
- [ ] Session persistence beyond layout (filters, call-graph depth per project)
