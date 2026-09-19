# RetDec Decompiler Plugin Sample

Minimal **post-decompile hook** for extending RetDec Imortek without modifying
core sources. RetDec is a specification-extraction decompiler (v2.0.22): the
primary artefacts are recovered C (buildable by default) plus
`.config.json` semantic detections. This sample shows how to run *after*
that pipeline.

This is the simplest working approach today: `retdec-decompiler.cpp` includes
LLVM `PluginLoader` but does **not** expose `--load-pass-plugin`. GUI plugins
use Qt `QPluginLoader`
([plugin_interface.h](../../include/retdec/gui/settings/plugin_interface.h)).

## What ships here

| File | Purpose |
|------|---------|
| [post_process_output.py](post_process_output.py) | Working hook: append a banner comment to decompiled `.c` |
| [plugin_load_probe.cpp](plugin_load_probe.cpp) | Shared-library stub that logs on load (for future LLVM/GUI loader wiring) |
| [CMakeLists.txt](CMakeLists.txt) | Optional build of the probe shared library |

There is no in-tree LLVM pass-plugin switch. Do not treat the probe as a
shipped loader.

## Quick start — post-process hook (recommended)

Build `retdec-decompiler` with a CMake preset (`full-linux-debug` on
Linux/WSL/macOS, `full-windows-debug` on Windows). Then:

```bash
# 1. Decompile as usual (buildable C is the default)
retdec-decompiler -o sample.c input.exe

# 2. Run the sample plugin (post-process)
python3 examples/decompiler_plugin/post_process_output.py sample.c

# sample.c now has a banner at the top
```

Wrap both steps:

```bash
retdec-decompiler -o out.c binary.exe && \
  python3 examples/decompiler_plugin/post_process_output.py out.c
```

Set `RETDEC_PLUGIN_POST=1` to print diagnostics to stderr.

For semantic sidecars, operate on `out.config.json` as well as `out.c`.
Input-keyed outputs (`.pyc` → Python, `.wasm` → WAT, …) can be post-processed
the same way; this sample only edits C comments.

## Building the load probe (optional)

The C++ probe demonstrates a shared object whose constructor runs at `dlopen` /
`LoadLibrary` time — the same mechanism LLVM `PluginLoader` uses for pass
plugins. RetDec does not wire that path yet.

```bash
cmake -S examples/decompiler_plugin -B build/plugin-sample
cmake --build build/plugin-sample
```

On Linux / macOS:

```bash
python3 -c "import ctypes; ctypes.CDLL('build/plugin-sample/libretdec_plugin_probe.so')"
```

Expected stderr:

```
[retdec-plugin-probe] shared library loaded
```

On Windows, use `build/plugin-sample/Release/retdec_plugin_probe.dll` with
`ctypes.WinDLL`.

## Future: LLVM pass plugin

When RetDec exposes `--load-pass-plugin PATH`, build a pass with the LLVM new
pass manager plugin API and register a module pass that logs in `run()`:

1. Implement `llvmGetPassPluginInfo()` exporting pass registration.
2. Link against the same LLVM version as RetDec (`deps/llvm`).
3. Pass the `.so`/`.dll` via LLVM's standard plugin flags (mirroring `opt -load-pass-plugin=...`).

Until then, prefer:

- **CLI:** `--profile fast|balanced|quality` or `--llvm-passes-json`
  ([profiles/README.md](../../src/retdec-decompiler/profiles/README.md))
- **Post:** `post_process_output.py` or your own script on `.c` / `.config.json`
- **GUI:** `IDecompilerPlugin` / `IAnalysisPlugin` via Qt
  ([plugin_interface.h](../../include/retdec/gui/settings/plugin_interface.h))

## Related

- [docs/pipeline_builder_schema.json](../../docs/pipeline_builder_schema.json)
- [docs/internal/ENGINEERING_ROADMAP.md](../../docs/internal/ENGINEERING_ROADMAP.md) — Tier 5 extensibility
