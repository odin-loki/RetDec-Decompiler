# Vendored first-party libraries

Both are Odin Loch / Imortek projects, AGPL-3.0 with a commercial option like
RetDec itself. They are copied in, not fetched, so every build and every CI job
works offline and needs no submodule checkout.

| Directory | Upstream | Commit | What is used |
|-----------|----------|--------|--------------|
| `fuseui/` | [odin-loki/FUSE](https://github.com/odin-loki/FUSE) | `5dcee19e1e8545324396adf07b4aa25292669dd9` (2026-10-30) | `Source/FUSE/UI` (FuseUI: text, core, widgets, doc, app shell) plus `vendor/stb`, `vendor/fonts/opensans`, `vendor/lua`. Paths are kept as upstream lays them out so FuseUI's own CMake resolves `vendor/` unchanged. The macOS (Cocoa) window backend in `Source/FUSE/UI/App/src/native_cocoa.mm` is RetDec's addition and is not upstream yet. |
| `forge/` | [odin-loki/FORGE](https://github.com/odin-loki/FORGE) | `b135941680664502fec59924f51d3c3dc11e23e9` (2026-11-07) | The whole tree (`git archive`). RetDec links the OS layer, core and support libraries and the inference stack; tests, apps and examples are switched off. |

To update one, replace the directory from the new upstream commit, keep any
local additions listed above, and change the commit here.
