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

## Remaining (optional micro-polish)
- [ ] Centralize all inline `#313244` panel styles into QSS
- [ ] Target panel in layout (arch/OS editor still orphan)
- [ ] i18n / Light theme QSS file
- [ ] Compare dialog non-modal + remember path
