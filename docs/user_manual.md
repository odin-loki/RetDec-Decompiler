# RetDec User Manual

This manual describes the **Qt 6 GUI** (`retdec-gui`), its panels, settings, and common workflows. For **building** RetDec from source (CMake presets, `build/linux` vs `build/windows`, CUDA, MSVC), read [BUILD_REFERENCE.md](BUILD_REFERENCE.md) and the platform guides [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) / [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md). The top-level [README.md](../README.md) lists supported file formats and CLI examples.

---

## Getting Started

### Installation

**Prerequisites**

- CMake **3.26** or later (see [CMakePresets.json](../CMakePresets.json))
- A C++17-capable compiler (GCC 11+, Clang 14+, or MSVC 2019+ on Windows)
- Qt 6 (6.4 or later) with Widgets, Core, Gui, **and Test** — **required** for the `full-linux-*` and `full-windows-*` presets that build `retdec-gui`
- Ninja (recommended)
- CUDA Toolkit 11.8+ recommended for full presets (GPU analysis + Qwen3); builds can configure without NVCC; runtime falls back to CPU if no suitable GPU is present

**Build (Linux/WSL — full GUI + CUDA preset)**

```bash
# Ubuntu/Debian: sudo apt install qt6-base-dev qt6-base-dev-tools
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
```

**Build (manual preset)**

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
```

**Install (from the same binary directory you built)**

```bash
# Typical system install:
cmake --install build/linux --prefix /usr/local

# Or use the preset’s default prefix under the repo:
cmake --install build/linux
```

After install, ensure `retdec-gui` is on your `PATH` (e.g. `/usr/local/bin`).

**Windows (native MSVC build)** — use the PowerShell scripts so MSVC, CUDA, and Qt are detected; binaries are usually run from `dist\windows\` after staging:

```powershell
.\scripts\windows_native_configure.ps1
.\scripts\windows_native_build.ps1
.\dist\windows\retdec-gui.exe
```

### Running the GUI

```bash
retdec-gui
```

Or pass a binary to open it directly:

```bash
retdec-gui /path/to/binary.elf
```

**WSL with Windows display:** if you do not have WSLg, use an X server on Windows (e.g. VcXsrv) and follow [scripts/launch_gui.sh](../scripts/launch_gui.sh) or [scripts/launch_gui_vcxsrv.sh](../scripts/launch_gui_vcxsrv.sh) comments.

---

## The Interface

### Main Window Layout

RetDec uses a dockable panel layout.  All panels can be rearranged,
floated, or closed via the **View** menu.

```
┌────────────────────────────────────────────────────────────────┐
│  Menu: File  Edit  View  Analysis  Tools  Help                 │
├──────────┬─────────────────────────────────┬───────────────────┤
│ Function │           TriPane Code View      │  Type Hierarchy   │
│  List    │  ┌─────────┐┌────────┐┌───────┐ │  & Call Graph     │
│          │  │Assembly ││ SSA IR ││Decomp.│ │                   │
│          │  └─────────┘└────────┘└───────┘ │                   │
├──────────┴──────────────────────────────────┤                   │
│ Strings & Constants Browser                 │                   │
├─────────────────────────────────────────────┤                   │
│ AI Assistant                                │                   │
├─────────────────────────────────────────────┴───────────────────┤
│ Progress: ████████████░░░░ 68% | Type inference (3.2s)          │
└────────────────────────────────────────────────────────────────┘
```

---

## Opening a Binary

1. **File → Open…** (Ctrl+O) — opens a file browser.
2. Select any ELF, PE, Mach-O, or raw binary file.
3. RetDec will automatically detect the architecture and format.
4. The analysis pipeline starts immediately.

### Supported input formats

| Format | Extensions | Notes |
|--------|-----------|-------|
| ELF    | `.elf`, `.so`, `.o` | Linux/Android |
| PE     | `.exe`, `.dll`, `.sys` | Windows |
| Mach-O | (no extension), `.dylib` | macOS/iOS |
| Raw    | `.bin` | Specify base address in settings |
| PTX    | `.ptx` | CUDA kernel assembly |
| WASM   | `.wasm` | WebAssembly binary |
| JVM bytecode | `.class`, `.jar` | Java 1–21+ |
| Android DEX  | `.dex`, `.apk` | Android bytecode |
| Python bytecode | `.pyc` | CPython 3.8–3.12 |
| Lua bytecode | `.luac` | Lua 5.1, 5.2, 5.3, 5.4 |

**Managed language inputs** (`.class`, `.dex`, `.pyc`, `.luac`, `.wasm`) bypass
the LLVM pipeline entirely — they are detected by magic bytes and dispatched
directly to the appropriate language-specific pipeline.

---

## Understanding the Interface

### Function List Panel

Displays all recovered functions sorted by address.  Each entry shows:

- **Name** — recovered or mangled symbol name (demangled if available)
- **Confidence badge** — coloured dot indicating recovery quality
  - 🟢 High (≥ 0.8)
  - 🟡 Medium (≥ 0.5)
  - 🔴 Low (< 0.5)
- **Pattern flags** — icons for detected patterns (STL, Crypto, Virtual, etc.)

**Filter** the list using the search box (supports regex toggle).
**Double-click** a function to navigate all panes to that function.

### TriPane Code View

Three synchronised editors:

| Left | Middle | Right |
|------|--------|-------|
| Assembly | SSA IR | Decompiled C |

Scrolling any pane scrolls all three in sync.  Click any line to highlight
the corresponding lines in the other two panes.

### CFG Visualiser

Shows the control-flow graph of the currently selected function.

- **Click** a basic block to select it; highlights the corresponding code.
- **Drag** to pan; **scroll wheel** to zoom.
- **Mini-map** (bottom-right) for navigating large CFGs.
- Loop regions are shown with a coloured overlay.
- **Export** as SVG via right-click menu.

### Type Hierarchy Browser

Shows the recovered C++ class hierarchy.

- Left tree: inheritance hierarchy (subclass → superclass arrows).
- Right table: vtable layout for the selected class.
- **Double-click** a class to navigate to its constructor.

### Call Graph Explorer

Interactive call graph.

- Strongly-connected components are collapsed into super-nodes (shown in
  a different colour).
- Module clusters are shown as background regions.
- **Filter** by function name prefix.

### Strings & Constants Browser

Two tabs:

**Strings tab** — all string literals with:
- Encoding (ASCII / UTF-8 / UTF-16)
- Inferred category (URL, FilePath, RegEx, FormatString, CryptoConst)
- Filter by category dropdown

**Constants tab** — numeric constants with:
- Size (8/16/32/64-bit)
- Semantic label (MagicNumber, CryptoKey, Port, FloatSpecial)

**Export CSV** button saves the table.

### Diff / Compare View

Compare decompiled output before and after a recovery pass.

1. Select a recovery stage from the **Stage** dropdown.
2. Before (left) and After (right) are shown side by side.
3. Lines are colour-coded: red = removed, green = added.
4. Use **▲ Prev** / **▼ Next** to jump between hunks.
5. **Export** as `.patch` or HTML report.

---

## Using the AI Assistant

### Loading a model

1. Open **Settings → ML tab**.
2. Set **Model file** to a `.gguf` file (Qwen3 recommended).
3. Select **Quantisation level** (Q4_K_M is a good balance of speed/quality).
4. Click **Apply**.
5. In the AI Assistant panel, click **Load Model**.

### Querying

Type a question in the input box and press Enter or click **Send**.

The assistant automatically receives:
- The currently selected function's decompiled code.
- Any additional context you type.

Example queries:
- "What does this function do?"
- "Is this a known crypto algorithm?"
- "Rewrite this as idiomatic C++17."

**Stop Generation** cancels the current response.
**Clear** resets the chat history.

---

## Exporting Results

**File → Export As…** offers:

| Format | Description |
|--------|-------------|
| C      | Recovered C source (default) |
| C++    | Recovered C++ with class hierarchy |
| F#     | F# functional approximation |
| VB.NET | Visual Basic .NET |
| WASM/WAT | WebAssembly text format |
| Lua    | Lua script (for Lua bytecode inputs) |

**Analysis → Save CMakeLists.txt** exports the module clustering result as a
`CMakeLists.txt` ready for compilation.

---

## Configuration Reference

Open **Settings** (Ctrl+,) or **Edit → Settings**.

### General Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Theme | Dark | Dark / Light / System Default |
| Editor font | Cascadia Code 10pt | Monospace font for code panes |
| Show line numbers | Yes | Display line numbers in code editors |
| Word wrap | No | Wrap long lines in decompiled output |
| Restore session | Yes | Re-open last binary on startup |

### Analysis Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Type inference | Enabled | Recover variable types |
| Pattern matching | Enabled | Match STL/crypto/libc patterns |
| Concurrency detection | Enabled | Find mutexes, threads, atomics |
| CUDA host recovery | Enabled | Recover CUDA runtime calls |
| Thread count | Auto | 0 = use all hardware threads |
| Max analysis time | 300s | Abort analysis after timeout |

### CUDA Tab

| Setting | Default | Description |
|---------|---------|-------------|
| CUDA device | Auto | GPU device index (0 = first NVIDIA GPU) |
| Use GPU | Yes | Prefer GPU over CPU for analysis kernels |
| Block size | 256 | CUDA thread block size per kernel launch |
| Enable profiling | No | Record CUDA event kernel timings |

> **Note:** If no CUDA-capable GPU is detected at runtime, all GPU analysis
> passes automatically fall back to multi-threaded CPU implementations with no
> user action required.

### ML Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Model file | (empty) | Path to `.gguf` model |
| Quantisation | Q4_K_M | Quantisation level |
| Inference device | Auto | CPU / GPU / Auto |
| Temperature | 0.7 | Sampling temperature |
| Top-P | 0.9 | Nucleus sampling threshold |
| Max new tokens | 512 | Maximum tokens per response |

### Recovery Tab

Individual toggles for each semantic detector, with per-detector confidence
thresholds.

### Advanced Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Verbosity | Normal | Quiet / Normal / Verbose / Debug |
| Dump IR | No | Write SSA IR to file after each stage |
| Dump CFG | No | Write CFG as DOT files |
| Max functions | All | 0 = decompile all functions |
| Demangle names | Yes | Demangle C++/D/Rust symbol names |

### Plugins Tab

Lists all installed plugins.  Use the checkbox to enable/disable each plugin
without unloading it.  Click **Install Plugin…** to load a `.so`/`.dll` plugin.

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| Ctrl+O | Open binary |
| Ctrl+, | Open settings |
| Ctrl+F | Search in current pane |
| Ctrl+G | Go to address |
| Ctrl+B | Go to function |
| F5 | Re-run analysis |
| F12 | Go to definition |
| Ctrl+Shift+C | Copy current function as C |
| Ctrl+Shift+D | Open Diff view for current function |
| Ctrl+Shift+A | Focus AI assistant |

---

## Command-line companion (`retdec-decompiler`)

The GUI and the **`retdec-decompiler`** CLI share the same analysis engines. After installation, run:

```bash
retdec-decompiler --help
```

Typical usage:

```bash
retdec-decompiler input.exe -o output.c
retdec-decompiler module.wasm -o module.wat
retdec-decompiler script.pyc -o script.py
```

Optional **Qwen3** GGUF for AI-assisted naming:

```bash
retdec-decompiler binary.elf --model /path/to/model.gguf -o output.c
```

On Windows staged builds:

```powershell
.\dist\windows\retdec-decompiler.exe --help
```

Full feature lists and format tables: [README.md](../README.md).

---

## Windows-specific notes

| Topic | Detail |
|-------|--------|
| **Portable folder** | `dist\windows\` after `windows_native_build.ps1` contains `retdec-gui.exe`, Qt platforms plugins, CUDA runtime DLLs if built with CUDA, and MSVC redistributables copied by the script. |
| **Debuggable bundle** | `windows_prepare_debuggable_gui.ps1` produces `dist\windows\debuggable\` with PDBs for RetDec targets; use for deep debugging. |
| **Smoke tests** | `.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows` |
| **VcXsrv / X11** | For GUI from WSL without WSLg, install an X server on Windows and set `DISPLAY` as described in `scripts/launch_gui_vcxsrv.sh`. |

---

## Troubleshooting (GUI)

| Problem | What to check |
|---------|----------------|
| GUI does not start | Run from `dist\windows` (or install prefix `bin`) so Qt plugins and `platforms\qwindows.dll` sit next to the executable; re-run `windeployqt` if you moved files manually. |
| “No CUDA” / slow analysis | In **Settings → CUDA**, confirm **Use GPU**; install an NVIDIA driver; full MSVC build required for Windows CUDA kernels (MinGW cross build is CPU-only for GPU passes). |
| AI panel idle | **Settings → ML** — set model path, **Apply**, then **Load Model** in the assistant panel. |
| Empty decompilation | Check **Settings → Advanced → Max functions** (0 = all). Very large binaries may hit **Max analysis time** on the Analysis tab. |
| Crash on open file | Try **File → Open** with a smaller sample; enable **Verbosity** under Advanced and capture console output; on Windows use `run_gui_with_procdump.ps1` (see [scripts/README.md](../scripts/README.md)). |

For **build** failures (OpenSSL, LLVM download, Qt, MSVC env), see [BUILD_REFERENCE.md](BUILD_REFERENCE.md#troubleshooting) and [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

---

## Documentation map

| Need | Document |
|------|----------|
| Build from source | [BUILD_REFERENCE.md](BUILD_REFERENCE.md) |
| MSVC + CUDA + Qt | [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) |
| All docs index | [docs/README.md](README.md) |
| Architecture / pipeline | [architecture.md](architecture.md) |
| Contributing code | [developer_guide.md](developer_guide.md) |
