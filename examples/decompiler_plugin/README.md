# RetDec Decompiler Plugin Sample

Minimal **post-decompile hook** for extending RetDec without modifying core sources. This is the simplest working approach today: `retdec-decompiler.cpp` includes LLVM `PluginLoader` but does **not** expose `--load-pass-plugin`; GUI plugins use Qt `QPluginLoader` ([plugin_interface.h](../../include/retdec/gui/settings/plugin_interface.h)).

## What ships here

| File | Purpose |
|------|---------|
| [post_process_output.py](post_process_output.py) | Working hook: append a banner comment to decompiled `.c` |
| [plugin_load_probe.cpp](plugin_load_probe.cpp) | Shared-library stub that logs on load (for future LLVM/GUI loader wiring) |
| [CMakeLists.txt](CMakeLists.txt) | Optional build of the probe shared library |

## Quick start — post-process hook (recommended)

```bash
# 1. Decompile as usual
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

## Building the load probe (optional)

The C++ probe demonstrates a shared object whose constructor runs at `dlopen` / `LoadLibrary` time — the same mechanism LLVM `PluginLoader` uses for pass plugins.

```bash
cmake -S examples/decompiler_plugin -B build/plugin-sample
cmake --build build/plugin-sample
```

On Linux:

```bash
# LLVM pass plugins are typically loaded by opt/llc; RetDec does not wire this yet.
# Manual load test:
python3 -c "import ctypes; ctypes.CDLL('build/plugin-sample/libretdec_plugin_probe.so')"
```

Expected stderr:

```
[retdec-plugin-probe] shared library loaded
```

On Windows, use `build/plugin-sample/Release/retdec_plugin_probe.dll` with `ctypes.WinDLL`.

## Future: LLVM pass plugin

When RetDec exposes `--load-pass-plugin PATH`, build a pass with the LLVM new pass manager plugin API and register a module pass that logs in `run()`:

1. Implement `llvmGetPassPluginInfo()` exporting pass registration.
2. Link against the same LLVM version as RetDec (`deps/llvm`).
3. Pass the `.so`/`.dll` via LLVM's standard plugin flags (mirroring `opt -load-pass-plugin=...`).

Until then, prefer:

- **CLI:** `--llvm-passes-json` custom pipelines ([profiles/README.md](../../src/retdec-decompiler/profiles/README.md))
- **Post:** `post_process_output.py` or your own script on `.c` / `.config.json`
- **GUI:** `IDecompilerPlugin` / `IAnalysisPlugin` via Qt ([plugin_interface.h](../../include/retdec/gui/settings/plugin_interface.h))

## Related

- [docs/pipeline_builder_schema.json](../../docs/pipeline_builder_schema.json)
- [docs/ENGINEERING_ROADMAP.md](../../docs/ENGINEERING_ROADMAP.md) — Tier 5 extensibility
