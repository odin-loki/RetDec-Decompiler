# Vendored first-party libraries

Both are Odin Loch / Imortek projects, AGPL-3.0 with a commercial option like
RetDec itself. They are copied in, not fetched, so every build and every CI job
works offline and needs no submodule checkout.

| Directory | Upstream | Commit | What is used |
|-----------|----------|--------|--------------|
| `fuseui/` | [odin-loki/FUSE](https://github.com/odin-loki/FUSE) | `5dcee19e1e8545324396adf07b4aa25292669dd9` (2026-10-30) | `Source/FUSE/UI` (FuseUI: text, core, widgets, doc, app shell) plus `vendor/stb`, `vendor/fonts`, `vendor/lua`. Paths are kept as upstream lays them out so FuseUI's own CMake resolves `vendor/` unchanged. RetDec additions, not upstream yet: see below. |
| `forge/` | [odin-loki/FORGE](https://github.com/odin-loki/FORGE) | `b135941680664502fec59924f51d3c3dc11e23e9` (2026-11-07) | The whole tree (`git archive`). RetDec links the OS layer, core and support libraries and the inference stack; tests, apps and examples are switched off. |

### FuseUI: RetDec additions (not upstream yet)

All additive and source compatible (new enum values are appended, new struct fields come last with defaults); each
has gate tests in `Source/FUSE/UI/tests` (`fuse_ui_extra_*`, `test_ui_extras.cpp`; `fuse_app_extra_*`,
`test_app_extras.cpp`; `fuse_app_native_extras`, an X11 window under Xvfb).

- **Window backends**: the macOS (Cocoa) backend (`App/src/native_cocoa.mm`, `cocoa_bridge.hpp`, wired in
  `App/CMakeLists.txt` and `native.cpp`) and a software presenter for X11 in `native.cpp` (upstream draws an X11
  window only through Vulkan).
- **Several font faces**: CSS `font-family` (list, quoted names, generic families through aliases) and `font-weight`
  select a face per node (labels, lineedit, codeedit, any widget text); text layout, measurement
  (`UiContext::measureString(s, px, node)`, `lineHeight(px, node)`) and the one glyph atlas handle every face.
  `UiContext::registerFont / registerSharedFont / registerFontFile / setFontAlias / setDefaultFontFamily /
  addFallbackFont / registerBundledFonts / resolveFont / fontFor`; `fuse::text::Font::load`, `addFontSearchPath`,
  `fontSearchPaths`, `findFontFile`, `bundledFaces`. Fonts stay files: the build tree's `vendor/fonts` is compiled in,
  an installed tree finds them at `<exe>/../share/fuse/fonts` (`FUSE_UI_FONT_INSTALL_REL`; RetDec sets
  `../share/retdec/gui/fonts` and installs them there from `cmake/first_party.cmake`), plus `$FUSE_UI_FONT_DIR`.
  `codeedit` is monospace by default; `fuse_app` windows register the bundled faces.
- **Bundled fonts**: `vendor/fonts/opensans/OpenSans-Bold.ttf` (Open Sans 1.10 Bold, Apache-2.0, same release as the
  regular face), `vendor/fonts/jetbrainsmono` (JetBrains Mono 2.304 Regular + Bold, OFL-1.1) and
  `vendor/fonts/dejavu/DejaVuSans.ttf` (DejaVu 2.37, Bitstream Vera / Arev licences) as the glyph-fallback face
  (check marks, arrows, geometric shapes); each directory has its licence and a `VERSION` file with source and
  SHA-256.
- **Glyph fallback**: `Font::bundledFallbacks()` ends with DejaVu Sans; the bundled faces and context-owned faces get
  the fallback chain, so symbols missing from a face render from any registered fallback face.
- **Placeholders**: `widgets::setPlaceholderText` (lineedit, codeedit) in CSS `placeholder-color` (default: the text
  colour at 45 %), shown while empty and unfocused; `UiContext::setPlaceholder / placeholder`.
- **Double click**: `SignalKind::DoubleClicked` and `EventType::DoubleClick` from press timing / distance
  (`UiContext::setDoubleClick`, default 500 ms / 4 px; `Event::clicks` counts the series), on the context clock
  (`UiContext::setClock / now`; fuse_app uses the application clock).
- **Item views**: table / model list multi-selection (`SelectionMode::Extended`: Ctrl / Shift click, Shift+arrows,
  Ctrl+A, Ctrl+Space; `tableSelectedRows`, `setTableRowSelected`, `tableSelectAll`, `tableClearSelection`,
  `SignalKind::SelectionChanged`), `scrollToRow(ui, table, row, hint)`, optional `SignalKind::Activated` on Enter /
  double click (`setTableActivation`), per-column (`setTableColumnFont`) and per-cell (`ItemModel::font`) fonts.
  Single selection stays the default.
- **Spin box / progress bar**: `setSpecialValueText` (shown at the minimum), `setProgressIndeterminate` (busy bar).
- **Cooked styles**: `font-family` values are cooked; custom properties are now tagged `0xFFFFFFFF` (blobs cooked
  before, which tagged them 56, still load).
- **Punctuation keys**: `Key::Minus, Equal, Comma, Period, Slash, Semicolon, Quote, Backslash, Grave` (appended after
  `Key::Other`; earlier values keep their numbers), mapped by key position in all three backends (X11 keysyms, with
  shifted symbols reported by their unshifted key plus Shift; Win32 `VK_OEM_*`; macOS `kVK_ANSI_*`, digits by position
  too). `keyName`, `keyFromName` (aliases such as `Esc`, `Backtick`, `Plus`) and `parseShortcut("Ctrl+Shift+`")`.
  Win32 no longer maps numpad virtual keys onto letters.
- **Cancellable close**: `AppWindow::setCloseRequestHandler(std::function<bool(AppWindow&)>)`; the OS close button
  (Win32 `WM_CLOSE`, X11 `WM_DELETE_WINDOW`, Cocoa `windowShouldClose`) calls `AppWindow::userCloseRequest()`, which
  asks the hook (false keeps the window, e.g. for an "unsaved changes?" prompt). `requestClose()` still closes
  unconditionally; without a hook nothing changes.
- **File drops**: `AppWindow::setFileDropHandler(std::function<void(AppWindow&, const FileDrop&)>)` with
  `FileDrop{paths (UTF-8), pos, target}`; `dropFiles(paths, pos)` for backends and tests; `parseUriList`. Win32
  `DragAcceptFiles` / `WM_DROPFILES`, an X11 XDND v5 drop target (Enter / Position / Status / Drop / Finished, the
  `XdndSelection` converted to `text/uri-list` through the runtime-loaded libX11), Cocoa
  `registerForDraggedTypes:NSPasteboardTypeFileURL` + `draggingEntered:` / `performDragOperation:` on the content view.
  Drags are refused while no hook is set. No drag-hover events yet.
- **Themeable widget parts**: `UiContext::partStyle(owner, part, states, defaults)` styles pieces a widget draws
  itself as if they were child elements (`StyleEngine::computeValue` computes them without interning, so it is safe
  during paint). Parts: `tableheader` (background, color, border-color = column separators, background-end = sort
  arrow), `tablestripe`, `tab` / `tab:selected` (background, color, border-radius, background-end = indicator),
  `tabline`, `dockclose` / `dockclose:hover` (color), `scrolltrack`, `scrollthumb` / `scrollthumb:pressed` (scroll
  containers, table / grid views and the `scrollbar` widget), and the code editor's `codearea`, `codegutter`
  (`:selected` = caret line), `codeselection`, `codecurrentline`, `codecaret`, `codeplain`, `codekeyword`,
  `codestring`, `codenumber`, `codecomment`. `docktitle` paints its own CSS background (class default `#2a2a33`) and
  `splithandle` its grip in `background-end` (themed per `:hover` / `:pressed` / `:focus`). The default theme lists
  every former built-in colour, so the dark look is unchanged (scroll tracks of plain scroll containers now use the
  table's `rgba(0,0,0,0.15)`); menus were already CSS-driven.
- **Build**: the UI1 key / IME tests that need the engine's `Source/FUSE/Core` are skipped when it is absent, and
  `fuse_text_fallback` skips (exit 77) without the Noto / Twemoji faces, so the vendored tree builds and tests on its
  own.

To update one, replace the directory from the new upstream commit, keep any
local additions listed above, and change the commit here.
