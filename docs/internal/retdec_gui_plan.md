# RetDec GUI — v3 Simplification (mockup → implementation)

## v3 — the "between" design

v2 (six right-dock tabs + mode toolbar) was too busy. v3 keeps the parts
that worked (central QTabWidget for documents, live console at the bottom)
and removes the parts that fought the user (mode strip, six workspace
tabs that mixed navigation with tools, scattered Inspect/Target/Sig
panels).

See `assets/retdec_gui_redesign_v3.png` for the visual target.

### What stays from v2

* **Central `QTabWidget`** as the document area (Decompiled C / Assembly /
  IR / CFG).
* **Live console** bottom-dock tab that streams every subprocess output.
* **Status bar** with file path, stage, function count, progress bar,
  elapsed.

### What returns from v1

* **Synced tri-pane code view** is back — but as **one centre tab**
  named "Synced (Asm ┃ IR ┃ C)", not a fifth dock that competes with
  the others. Users who want the side-by-side cross-highlight pick it
  from the document tabs.

### What v3 simplifies

| Region | v2 | v3 |
|---|---|---|
| Mode toolbar | 4 buttons (Inspect / Decompile / Sigs / IR) | **dropped** — modes were redundant with tabs |
| Right dock | 6 tabs (Inspect/Binary/Target/Strings/CG/Sigs) | **2 tabs**: Strings, Inspect |
| Target panel | a workspace tab | moved into **Settings → Target** |
| Signature Studio | a workspace tab | moved into **Tools → Signature Studio…** (separate window) |
| Call Graph | a workspace tab | moved into **Tools → Call Graph…** (separate window) |
| Binary Browser | a workspace tab | folded into **Inspect** tab |
| Bottom dock | Console / Cmd log / Diagnostics / Progress | Console / **Problems** (merged Diagnostics+Progress hint) / Command log |
| Triage | hidden in the Inspect panel | **Triage banner** at the top of the central area: arch / OS / size / packed badge + one-click `[Unpack first]` `[Decompile anyway]` `[More ▼]` `[✕]`, dismissible |

### Workflow this directs

1. **Open binary** → Triage banner appears with metadata + obvious next
   actions. Console tab auto-raises with live `retdec-fileinfo` output.
2. **Pick a function** in the left sidebar (always-visible Functions
   list with prominent filter).
3. **Read decompiled C** in the centre (the default tab). Switch to
   Assembly / IR / CFG / Synced as needed.
4. **Look at strings / re-inspect metadata** in the small right dock.
5. **Watch progress / fix problems** in the bottom dock.

No mode buttons, no dialogs that hide the work. The screen always
shows the artefact you're working on.

---

# RetDec GUI — v2 Redesign, Live Console & Automated Debug (kept for history)


This document supersedes the earlier "modes + dock tabs" plan. It is the
implementation contract for the v2 redesign requested by the user:

> "The Console option in the GUI doesn't show any actual output when it's
> decompiling until after it has finished decompiling. Also I want the GUI
> completely redone. I want the tri-view panel scrapped and the view
> tabinated more. I want a lot better and ergonomic view."

It also adds graphical-debugging automation (Python-driven, debugger-attached
test runs) and the final test/bugfix sweep needed to call this GUI
"production ready".

---

## 0. Acceptance criteria (what "done" means)

1. **Live console.** Every line written by `retdec-decompiler`, `retdec-fileinfo`,
   `retdec-unpacker`, `retdec-bin2pat`, `retdec-pat2yara` appears in the GUI
   within ~100 ms of the child process flushing it — *during* the run, not after.
2. **Tri-pane gone from layout.** The `TriPaneCodeView` widget is no longer
   instantiated, docked, or referenced from the main toolbar/menu. (Source
   files and unit tests stay for one release so the public API is not broken,
   but they are excluded from the executable's panel set.)
3. **Tabbed, ergonomic main window.** The application uses:
   * **Central widget** = persistent `QTabWidget` of *document* tabs
     (Decompiled C / Assembly / IR / CFG / Console / Diff). The central widget
     is *not* a `QMainWindow` of tabified docks — that was the source of the
     "I can't find my tab" complaint.
   * **Left dock** = Functions + Types (tabbed inside one dock).
   * **Right dock** = Inspect + Strings + Imports + Target + Signatures
     (tabbed inside one dock).
   * **Bottom dock** = Console (live) + Command log + Diagnostics + Progress
     (tabbed inside one dock).
   * **Mode toolbar** (Inspect / Decompile / Signatures / IR) controls which
     right-dock + bottom-dock tabs are *raised*, not which docks exist.
   * **Status bar** with file path, current stage, elapsed time, function
     count, indeterminate-while-running progress bar.
4. **No regressions.** All existing `retdec-gui-tests` pass; the GUI launches
   in headless mode without warnings; a real decompilation against
   `tests/test_binaries/...` produces decompiled C in the centre tab.
5. **Bug sweep.** Issues uncovered during the redesign (race in
   `stopAnalysis`, never-hooked `onAnalysisStageChanged`, `std::size`
   indexing in tri-pane palette, etc.) are fixed.
6. **Automated graphical debug.** `scripts/python/gui_debug_driver.py` can:
   * Launch `retdec-gui` under `cdb`/`gdb` (auto-detected),
   * Send it a binary to decompile,
   * Capture stdout/stderr live to a log,
   * On hang or crash, dump a minidump (Windows) or core (Linux) and a
     backtrace,
   * Take periodic offscreen screenshots when run with `--shots`.
7. **Debugger-attached test run.** `scripts/run_gui_tests_under_debugger.{ps1,sh}`
   runs `retdec-gui-tests` under cdb/gdb, captures any crash with a
   backtrace, and exits non-zero if anything crashes.

---

## 1. Live console architecture

### 1.1 Root cause of the current bug

`RetDecMainWindow::onRunFullAnalysis` (in `src/gui/mainwindow.cpp`) starts
`decompilerProc_` with `MergedChannels` and only reads `readAllStandardOutput`
inside `onDecompilerProcessFinished`. There is **no** `readyReadStandardOutput`
connection, so nothing is shown until exit. Same pattern in
`InspectPanel::runFileinfo` / `onUnpackClicked` (output also batched at end).

### 1.2 Fix

* New panel `LiveConsolePanel` (`include/retdec/gui/panels/live_console_panel.h`,
  `src/gui/panels/live_console_panel.cpp`):
  * `QPlainTextEdit` with monospace font, dark Catppuccin tint, ANSI strip
    (very small filter — most retdec tools don't emit colour but mingw can).
  * Bounded scrollback (default 50 000 blocks; configurable).
  * Public slots: `attachProcess(QProcess*, label)`, `detachProcess(QProcess*)`,
    `appendBanner(tool, args, cwd)`, `appendLine(stream, text)`, `clear`.
  * Per-process line buffer with rate-limited flush (250 Hz max) via a
    `QTimer` so a chatty pass doesn't melt the UI thread.
* `RetDecMainWindow`:
  * Hook `decompilerProc_::readyReadStandardOutput` and `readyReadStandardError`
    *before* `start()`.
  * Use `QProcess::SeparateChannels` so we can colour stderr red.
  * Push live bytes to `LiveConsolePanel::appendChunk(stream, bytes)`.
  * Add `dockBottom_` tab "Console" alongside Diagnostics / Command log /
    Progress; auto-raise on start.
* `InspectPanel`:
  * Same treatment for `fileinfoProc_` and `unpackProc_` — emit a new signal
    `chunkReady(tool, stream, bytes)` that the main window forwards into the
    live console.

### 1.3 Threading model

All `QProcess` signals fire on the GUI thread → no extra locking needed.
The `LiveConsolePanel`'s flush timer is also GUI-thread. We keep a small
per-stream `QByteArray` buffer and append in chunks to avoid one
`appendPlainText` per byte (which is the actual perf bug behind chatty
`--print-after-all` runs).

---

## 2. Window redesign

### 2.1 New shell

```
┌───────────────────────────────────────────────────────────────────────────┐
│ File  Edit  Analysis  View  Tools  Help                                   │
├───────────────────────────────────────────────────────────────────────────┤
│ [Open] [Run▼] [Stop]  │  ◉ Inspect ◯ Decompile ◯ Signatures ◯ IR        │
├──────────┬───────────────────────────────────────────────┬────────────────┤
│          │                                               │                │
│ FUNCTIONS│  ┌─Decompiled C─┬─Assembly─┬─IR─┬─CFG─┬─Diff┐│  INSPECT       │
│ ─────────│  │                                          ││  STRINGS       │
│ Types    │  │   active document tab                    ││  IMPORTS       │
│          │  │                                          ││  TARGET        │
│  (tab    │  │                                          ││  SIGNATURES    │
│   bar)   │  │                                          ││                │
│          │  └──────────────────────────────────────────┘│  (tab bar)     │
│          │                                               │                │
├──────────┴───────────────────────────────────────────────┴────────────────┤
│  ┌─Console─┬─Command log─┬─Diagnostics─┬─Progress─┐                      │
│  │ live stdout/stderr…                            │                       │
│  │                                                 │                       │
│  └────────────────────────────────────────────────┘                       │
├───────────────────────────────────────────────────────────────────────────┤
│ binary.exe  │  Stage: Type inference …  │  0 fn  │  [██░░░] indet  │ 12s │
└───────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Why central `QTabWidget` (not central tabified docks)

`tabifyDockWidget` creates *floating-capable* tabs that hide each other when
the user resizes wrongly — that's the "tri-view panel" feel the user wants
gone. A real `QTabWidget` keeps the active document predictable, mirrors
IDE conventions (VSCode / Qt Creator / Ghidra), and lets `Ctrl+Tab` /
`Ctrl+1..5` work naturally.

### 2.3 Persistence

* `QSettings("retdec","retdec-gui")` keys:
  * `geometry`, `windowState`, `centralCurrentTab`,
    `dockLeftCurrentTab`, `dockRightCurrentTab`, `dockBottomCurrentTab`,
    `workMode`, `recentFiles`, `lastProjectSavePath`.
* On restore, central tab index is clamped to valid range; if `windowState`
  is empty (first run), defaults apply.

### 2.4 Mode toolbar (kept, simplified)

* Inspect → raise right-dock "Inspect" tab + central "Console" tab.
* Decompile → raise right-dock "Strings" + central "Decompiled C".
* Signatures → raise right-dock "Signatures" + central "Decompiled C".
* IR Workbench → raise right-dock "Inspect" + central "IR".

---

## 3. Bug fixes (sweep)

| # | Location | Bug | Fix |
|---|----------|-----|-----|
| 1 | `mainwindow.cpp::stopAnalysis` | Uses `waitForFinished(8000)` on GUI thread; freezes UI if child hangs. | Send `terminate()` first, then schedule `kill()` via a 2 s `QTimer`, drive UI with `processEvents` between checks. |
| 2 | `mainwindow.cpp::onAnalysisFinished` | `analysisBridge_::analysisCompleted` never fires for the external-process flow, so this slot is dead and the spinner can stick if `progressPanel_` is shown alone. | Bridge is fine for in-proc flows; emit explicit completion from `onDecompilerProcessFinished`. |
| 3 | `mainwindow.cpp::onAnalysisStageChanged` | Slot exists but is never `connect`-ed. | Wire it via `analysisBridge_::stageStarted`. |
| 4 | `tri_pane_code_view.cpp` (kept but fixed) | `std::size(palette)` on a `static const QColor[]` is fine, but the indexing `int idx = idx % static_cast<int>(...)` is unguarded against empty — add `Q_ASSERT`. | Trivial. |
| 5 | `command_log_panel.cpp::appendRun` | `text_->appendPlainText(line)` followed by manual `setTextCursor(End)` — `appendPlainText` already moves to end; the extra cursor move resets selection. | Drop redundant cursor move. |
| 6 | `inspect_panel.cpp::onUnpackClicked` | `unpackOutEdit_` "default" only re-applies if previously cleared; if user types whitespace, we still fall through with an empty path *after* the early return because `updateUnpackOutputDefault` runs again. Logic is reversed. | Restructure: trim once, fall back to default if empty. |
| 7 | `mainwindow.cpp::createMenus` | Recent-file actions reuse the same slot but rebuild every menu open; QActions leak via `addAction`'s ownership chain — harmless but noisy. | Use `recentMenu_->clear()` (already there) and `deleteLater()` not needed (parented to menu). Add Edit menu (Copy/Find in console + decompiled C). |
| 8 | `mainwindow.cpp::dropEvent` | Only consumes first URL silently — emit a status message and only accept files (not directories). | Filter. |
| 9 | `mainwindow.cpp::closeEvent` | `decompilerProc_->state()` not checked when prompting; if a run is alive *and* there are unsaved changes, the save dialog appears under the running spinner. | Stop analysis first if user confirms exit. |
| 10 | `progress_panel.cpp::renderWaterfall` | Divides by `viewW` when widget isn't yet shown (returns 0). | Guard `viewW > 1`. |

---

## 4. Automated graphical debug (Python)

`scripts/python/gui_debug_driver.py`:

```text
usage: gui_debug_driver.py [-h] [--gui-exe PATH] [--binary PATH] [--shots]
                            [--timeout SECONDS] [--out DIR]
                            [--debugger {auto,cdb,gdb,none}]
```

* Auto-locates `retdec-gui` next to itself or via `--gui-exe`.
* On Windows: spawns under `cdb -g -G` with a logfile sink and dump-on-fault
  enabled (`.dump /ma`).
* On Linux: spawns under `gdb -batch -ex run -ex "bt full"`.
* Sets `RETDEC_GUI_HEADLESS=1` and uses `--headless-exit-ms` for short runs
  unless `--no-headless` is given.
* With `--shots`: every 2 s saves a `QScreen::grabWindow` PNG via a tiny Qt
  helper invoked through `qttest`-style `QSocketNotifier` — actually, we
  ship a built-in flag `--screenshot-dir` in the GUI binary that writes
  PNGs on a `QTimer`. Both routes documented.
* Emits a final JSON report (`report.json`): exit code, durations, dump
  paths, screenshot paths, parsed backtrace summary.

The Python script is intentionally small (~300 LOC) and dependency-free
(stdlib only) so it can run on CI without `pip install`.

---

## 5. Tests (under debugger when run via the helper)

* New `tests/gui/live_console_panel_test.cpp`:
  * Constructs panel, attaches a `QProcess` running `cmd /c echo`, asserts
    text appears within 500 ms (event-loop driven via `QTest::qWait`).
  * Rate-limit test: simulated 1 000 chunks in 100 ms produces ≤ 25 UI
    repaints (count via `QObject::eventFilter` on the editor).
  * Bounded scrollback test: 200 000 lines result in ≤ 50 000 blocks.
* New `tests/gui/mainwindow_layout_test.cpp`:
  * Verifies central `QTabWidget` exists, has Decompiled C / Assembly / IR /
    CFG / Console / Diff tabs in order.
  * Mode buttons raise correct right-dock tabs.
  * `Ctrl+1..5` switches central tab.
* Updated `tri_pane_test.cpp`: still constructs widget directly (it's still
  in `retdec-gui-panels` for API stability); ensure no main-window
  reference.

---

## 6. Phased delivery (this PR/branch)

| Phase | Files touched (new/edited) | Test gates |
|------|----------------------------|------------|
| P1 — Live console | `live_console_panel.{h,cpp}` (new), `mainwindow.{h,cpp}`, `inspect_panel.{h,cpp}`, `CMakeLists.txt` | new `live_console_panel_test.cpp` |
| P2 — Layout redo | `mainwindow.{h,cpp}` (large delta), no panel rewrites | new `mainwindow_layout_test.cpp` |
| P3 — Bug sweep | targeted edits per table in §3 | existing tests still green |
| P4 — Debug driver | `scripts/python/gui_debug_driver.py`, `scripts/run_gui_tests_under_debugger.{ps1,sh}` | smoke run on CI |
| P5 — Polish | About text, recent-files UX, status-bar elapsed format | – |

Everything ships in one branch (`gui/v2-redesign-live-console`) and the
`build/windows` + `build/linux` Ninja directories rebuild incrementally —
no full reconfigure required.
