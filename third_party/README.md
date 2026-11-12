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
has gate tests in `Source/FUSE/UI/tests` (`fuse_ui_extra_*`, `test_ui_extras.cpp`).

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
- **Build**: the UI1 key / IME tests that need the engine's `Source/FUSE/Core` are skipped when it is absent, and
  `fuse_text_fallback` skips (exit 77) without the Noto / Twemoji faces, so the vendored tree builds and tests on its
  own.

To update one, replace the directory from the new upstream commit, keep any
local additions listed above, and change the commit here.
