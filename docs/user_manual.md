# RetDec User Manual

This manual describes the **Qt 6 GUI** (`retdec-gui`), its panels, settings, and common workflows for **RetDec Imortek 2.0.24**. For **building** from source, read [BUILD_REFERENCE.md](BUILD_REFERENCE.md) and the platform guides [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) / [MINGW_CROSS_DEEP_DIVE.md](MINGW_CROSS_DEEP_DIVE.md). The top-level [README.md](../README.md) lists supported file formats and CLI examples.

Decompilation in the GUI is the same `retdec-decompiler` subprocess as the CLI. Algorithm recovery, semantic export, and optional offline neural refinement are the product; recovered C is a supporting artefact.

---

## Getting Started

### CMake versions

| File | Minimum |
|------|---------|
| [CMakeLists.txt](../CMakeLists.txt) | **3.13** (`cmake_minimum_required`) |
| [CMakePresets.json](../CMakePresets.json) | **3.26** (`cmakeMinimumRequired`) |

Use 3.26+ if you configure with `cmake --preset …`. A raw `cmake -S . -B <dir>` configure is accepted by 3.13+. CUDA acceleration is **optional and off**: `RETDEC_ENABLE_CUDA_ACCEL` defaults **OFF** (including `full-linux-*` / `full-windows-*` presets) and is not linked from `src/retdec`.

### Installation from a GitHub Release

Prebuilt trees are on
[v2.0.24](https://github.com/odin-loki/RetDec-Decompiler/releases/tag/v2.0.24).
See [../QUICKSTART.md](../QUICKSTART.md) for exact asset names.

| Platform | What you get |
|----------|----------------|
| Linux x86_64 | `retdec-2.0.24-linux-x64.tar.gz` with `bin/retdec-gui` |
| macOS arm64 | `retdec-2.0.24-macos-arm64.tar.gz` with **`RetDec.app`** plus `bin/` |
| Windows | `retdec-2.0.24-windows-x64-setup.exe` (NSIS) and/or `retdec-2.0.24-windows-x64-portable.zip` |

macOS: the bundle is ad-hoc signed, **not notarised**. `install.sh` inside the tarball strips `com.apple.quarantine`. GitHub also uploads loose `install-macos.sh` so it does not collide with Linux `install.sh`. Launch with `open RetDec.app`.

Linux AppImage (`retdec-2.0.24-x86_64.AppImage`) is **opt-in** in
`release-installers.yml` (`APPIMAGE` defaults to `0`) and is often absent.

### Build from source (GUI presets)

**Prerequisites**

- CMake as in the table above
- A C++17-capable compiler (GCC 11+, Clang 14+, or MSVC 2019+ on Windows)
- Qt 6 (6.4 or later) with Widgets, Core, Gui, **and Test** — **required** for the `full-linux-*` and `full-windows-*` presets that build `retdec-gui`
- Ninja (recommended)
- CUDA Toolkit is **not** required

**Linux / WSL / macOS — full GUI preset** (binary dir `build/linux/`)

```bash
# Ubuntu/Debian: sudo apt install qt6-base-dev qt6-base-dev-tools
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
```

```bash
cmake --preset full-linux-release
cmake --build --preset full-linux-release
```

**Install (from the same binary directory you built)**

```bash
cmake --install build/linux --prefix /usr/local
```

After install, ensure `retdec-gui` is on your `PATH` (e.g. `/usr/local/bin`).
On macOS a relocatable `RetDec.app` is produced by
`scripts/build-macos-installer.sh`, not by `cmake --install` alone.

**Windows (native MSVC)** — binaries are usually run from `dist\windows\` after staging:

```powershell
.\scripts\windows_native_configure.ps1
.\scripts\windows_native_build.ps1
.\dist\windows\retdec-gui.exe
```

### Running the GUI

```bash
retdec-gui
retdec-gui /path/to/binary.elf
```

Headless / CI:

```bash
retdec-gui --headless
retdec-gui --headless-decompile /path/to/binary.elf
```

`--headless` uses Qt offscreen (`RETDEC_GUI_HEADLESS=1` is equivalent).
`--headless-decompile` requires a binary path. Optional
`--headless-exit-ms N` quits after N ms.

**WSL with Windows display:** if you do not have WSLg, use an X server on Windows (e.g. VcXsrv) and follow [scripts/launch_gui.sh](../scripts/launch_gui.sh) or [scripts/launch_gui_vcxsrv.sh](../scripts/launch_gui_vcxsrv.sh) comments.

---

## The Interface

There is **no Edit menu**. Menus are **File**, **Analysis**, **View**, **Tools**, **Help**.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Menu: File  Analysis  View  Tools  Help                                    │
├──────────┬───────────────────────────────────────────────┬──────────────────┤
│ Functions│ [Decompiled C][Assembly][IR][CFG][Synced]    │ Strings          │
│  (dock)  │          (centre document tabs)               │ Inspect          │
│          │                                               │ Binary           │
│          │                                               │ Target           │
├──────────┴───────────────────────────────────────────────┴──────────────────┤
│ [Console] [Problems] [History] [Progress]   (Output dock; Progress starts hidden) │
└─────────────────────────────────────────────────────────────────────────────┘
```

On first launch the centre is an empty state (“Open a binary to begin”) until
**File → Open Binary…**. A triage banner sits above the centre stack.

Docks (View menu toggles):

| Dock | Contents |
|------|----------|
| **Functions** (left) | Function list |
| **Workspace** (right) | Tabs: Strings, Inspect, Binary, Target |
| **Output** (bottom) | Tabs: Console, Problems, History, Progress |

Tools windows (not docks): Type Hierarchy, Call Graph, Signature Studio,
Compare, AI Assistant. Settings and Batch Decompile are dialogs.

Active GUI work: [GUI_ROADMAP.md](internal/GUI_ROADMAP.md).

---

## Opening a Binary

1. **File → Open Binary…** (Ctrl+O), drag-and-drop, or pass a path on the command line.
2. The file dialog filters `*.exe *.dll *.elf *.so *.dylib *.bin`; **All Files (*)** covers `.wasm`, `.class`, `.pyc`, `.luac`, and the rest.
3. Opening **does not** start decompilation. The GUI loads the binary, runs **fileinfo** for the Binary tab, and tries **cache reuse** of existing `.c` / `.dsm` / `.ll` artifacts. Status shows `not decompiled` or `cached`.
4. Press **F5** (**Analysis → Run Full Analysis**) to run `retdec-decompiler`.

**File → Open Project…** (Ctrl+Shift+O) loads a `.retdec` project file.

### Supported input formats

Same loaders as the CLI. Native CPU maturity is not “file opens”:

| Format | Extensions | Notes |
|--------|-----------|-------|
| ELF    | `.elf`, `.so`, `.o` | Linux/Android |
| PE     | `.exe`, `.dll`, `.sys` | Windows |
| Mach-O | (no extension), `.dylib` | macOS/iOS |
| Raw    | `.bin` | **Analysis → Treat input as raw image**; set arch/endian/bit-size |
| WASM   | `.wasm` | WebAssembly |
| JVM bytecode | `.class`, `.jar` | Managed path |
| Android DEX  | `.dex`, `.apk` | Managed path |
| .NET CIL | `.dll` (managed) | Managed path |
| Python bytecode | `.pyc` | Managed path |
| Lua bytecode | `.luac` | Lua 5.1–5.4 |

**Managed language inputs** (`.class`, `.dex`, `.pyc`, `.luac`, `.wasm`, CIL)
bypass the LLVM pipeline and dispatch to format-specific emitters.
Native binaries emit **C**. `--output-lang cpp` is rejected.

---

## Understanding the Interface

### Functions dock (left)

Recovered functions with name, address, a **confidence bar** (green ≥ 0.75,
yellow ≥ 0.40, red below), and pattern badges (STL, crypto, algo, design,
library). Double-click / select navigates Assembly, IR, Decompiled C, CFG,
Call Graph, and the Synced tab.

Filter bar: wildcard **Search…** (not a regex toggle), address range,
**Conf≥** spin, and STL / Crypto / Algo / Pat / Lib checkboxes. CSV / JSON /
Copy export the filtered list.

### Centre tabs (Decompiled C · Assembly · IR · CFG · Synced)

**Decompiled C**, **Assembly**, and **IR** are whole-file views from
decompiler artifacts (`.c`, `.dsm`, `.ll`). **CFG** is the selected
function. **Synced** is a tri-pane (Assembly | SSA IR | Decompiled C).

Ctrl+1 … Ctrl+5 switch those tabs. **View → Go to address…** is Ctrl+G.

### CFG visualiser

Toolbar: Fit, 100%, Expand chains, **Export SVG**, **Export PNG**.
SVG export requires Qt Svg (`QT_SVG`); otherwise the button warns.
Click a block to select; drag to pan; scroll to zoom. A mini-map is
shown for large graphs.

### Type Hierarchy / Call Graph / Signature Studio / Compare

Opened from **Tools**, not as permanent docks:

- **Type Hierarchy…** — recovered C++ class tree and vtable table.
- **Call Graph…** — interactive graph; large SCCs collapse to super-nodes
  (Tarjan).
- **Signature Studio…** — static-code signatures for the active binary.
- **Compare…** / **Compare original vs refined…** — side-by-side diff
  (**▲ Prev** / **▼ Next**; export `.patch` or HTML). Myers diff is used
  in `DiffPanel`.

### Strings (Workspace tab)

Two inner tabs: **Strings** and **Constants**, with CSV export on the
constants table. Categories are inferred (URL, FilePath, format string,
and similar labels in the panel).

### Inspect / Binary / Target

**Inspect** runs `retdec-fileinfo`. **Binary** is the hex/section browser
(context menu: re-run fileinfo, copy hashes, decompile a range).
**Target** edits architecture / format hints for the project.

---

## Analysis menu (maps to CLI flags)

| Action | CLI equivalent |
|--------|----------------|
| Run Full Analysis (F5) | `retdec-decompiler` child |
| Re-decompile Selected Function | `--select-functions` |
| Fast decompile (skip backend optimisations) | `--backend-no-opts --disable-static-code-detection` |
| Print LLVM IR after/before every pass | `--print-after-all` / `--print-before-all` |
| Keep unreachable functions | `-k` / `--keep-unreachable-funcs` |
| Delete intermediates | `--cleanup` |
| Try emulation unpacking | `--try-emulation` |
| Keep library functions | `--backend-keep-library-funcs` |
| Set max memory / No memory limit | `--max-memory` / `--no-memory-limit` |
| C output style… | backend flags (brackets, renaming, call-info, disabled/enabled opts) |
| Treat input as raw image | `-m raw` |
| Set PDB / static-code signature | `-p` / `--static-code-sigfile` |
| Variable renamer submenu | `--backend-var-renamer` |
| Stop Analysis (F6) | terminate child / cancel batch |

**Analysis → Fast decompile** is a quality trade-off, not a measured “~24%
faster” figure (that claim is withdrawn).

---

## AI-assisted analysis

**Tools → AI Assistant…** opens `AIAssistantPanel` (not a dock).

Optional llama.cpp refine uses env vars, not a CLI `--model` flag:

1. **Tools → Settings… → ML** — set **Model file (.gguf)**. When that path
   exists, Run Full Analysis sets `RETDEC_NEURAL_REFINE` /
   `RETDEC_NEURAL_MODEL` on the child. CPU device forces
   `RETDEC_NEURAL_N_GPU_LAYERS=0`; GPU/Auto use `-1`. Empty path leaves
   refinement off.
2. Or set the same variables on the CLI:

```bash
RETDEC_NEURAL_REFINE=1 RETDEC_NEURAL_MODEL=/path/to/model.gguf \
  retdec-decompiler binary.elf -o output.c
```

There is no `retdec-qwen3-runner` binary. The compile gate is
`cc`/`gcc -fsyntax-only`; decompiled C is not executed.

---

## Exporting Results

| Menu | What it does |
|------|----------------|
| **File → Save Decompiled…** (Ctrl+S) | Write decompiled C |
| **File → Export CMakeLists.txt…** | Skeleton `CMakeLists.txt` that `add_executable`s the recovered `.c` |
| **File → Export Decompile Bundle…** | ZIP of artifacts + decompiler command text |
| **File → Export Threat Intel…** | Threat-intel export from the current project |
| **File → Export As** | Rows from loaded **output plugins** (`IOutputPlugin`); empty shows “(no output plugins loaded)” |
| **File → Batch Decompile…** | Queue of binaries (dialog) |

Native pipeline output is C. Other languages are format-keyed managed
emitters, not a free-choice list of eleven languages.

---

## Configuration Reference

Open **Tools → Settings…** (Ctrl+,). There is no Edit → Settings.
Tabs: **General**, **Analysis**, **CUDA**, **ML**, **Recovery**,
**Advanced**, **Decompiler**, **Plugins**. OK / Cancel / Apply /
Restore Defaults, plus **Export…** / **Import…** JSON.

The Analysis tab banner is accurate: **F5 runs `retdec-decompiler`
externally; those toggles affect in-process analysis only.**

### General Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Theme | Dark | Dark / Light / System Default (Catppuccin Mocha / Latte) |
| Editor font | Cascadia Code, JetBrains Mono, Consolas 10pt | Monospace for code panes |
| Language | English (en) | en / de / fr / es / zh (restart) |
| Show line numbers | Yes | Code editors |
| Word wrap | No | Decompiled output |
| Restore last session | Yes | Re-open last **File → Open Binary** path |

### Analysis Tab (in-process; not the F5 child)

| Setting | Default | Description |
|---------|---------|-------------|
| Type inference | On | In-process stage |
| Pattern matching | On | In-process stage |
| Concurrency detection | On | In-process stage |
| CUDA host recovery | On | CPU-side host API detection (`ptx_decompile`), not `cuda_accel` |
| Serialisation detection | On | In-process stage |
| Module clustering | On | In-process stage |
| C++ lifter | On | In-process stage |
| Confidence minima | 0.5 / 0.6 / 0.4 | Type / pattern / recovery |
| Max analysis time | 300 s | 0 = Unlimited |
| Thread count | Auto (0) | `hardware_concurrency` |

### CUDA Tab

These widgets persist `CUDASettings`. They do **not** drive `cuda_accel` or
decompiler GPU kernels (`C-CUDA-PIPE` withdrawn). Neural GPU offload is
llama.cpp `n_gpu_layers` via the ML tab.

| Setting | Default | Description |
|---------|---------|-------------|
| CUDA device | Auto | Persisted preference |
| Prefer GPU over CPU | Yes | Persisted; does not enable decompiler GPU kernels |
| Work group size | 256 | Persisted |
| Kernel cache dir | (empty) | Persisted path |
| Enable cl_event profiling | No | Maps to `RETDEC_PROFILE_JSON` on the child; not CUDA kernel timings |

### ML Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Model file (.gguf) | (empty) | Path; empty leaves refine off |
| Quantisation | Q4_K_M | Q4_0 / Q4_K_M / Q5_K_M / Q6_K / F16 / F32 |
| Inference device | Auto | CPU / GPU / Auto |
| Temperature | 0.7 | Sampler |
| Top-P | 0.9 | Sampler |
| Top-K | 40 | Sampler |
| Max new tokens | 512 | Cap |
| Context length | 4096 | Passed to the child |

### Recovery Tab

Per-detector toggles and confidence spins: STL, crypto, patterns,
concurrency, CUDA host, RTTI, exceptions, virtual.

### Advanced Tab

| Setting | Default | Description |
|---------|---------|-------------|
| Verbosity | Normal | Quiet / Normal / Verbose / Debug |
| Dump SSA IR / assembly / CFG (DOT) / SSA after each stage | Off | Dump checkboxes |
| Colour terminal output | On | |
| IR dump path / intermediate dir | (empty) | Browse |
| Max functions | All (0) | 0 = all |
| Demangle symbol names | Yes | |

### Decompiler Tab

Passed to the `retdec-decompiler` child on Run Full Analysis:

- **Decompile profile** — `--profile` `fast` / `balanced` / `quality` (default balanced). Analysis → Fast decompile overrides to fast for the next run.
- Custom LLVM pass list — `--llvm-passes-json` when enabled.
- Output directory for `.gui-decompiled.*` (empty = beside the binary).
- Preferred `--output-lang` for native binaries: c / python / csharp / java / wat (default **c**). Managed inputs ignore this.
- Stream decompiler log to Console.
- Optional `--config` JSON path.

### Plugins Tab

Installed plugins with enable checkboxes, search paths, and **Install Plugin…**
(`.so` / `.dll`). Types: `IDecompilerPlugin`, `IOutputPlugin`,
`IVisualisationPlugin`, `IAnalysisPlugin`.

---

## Keyboard Shortcuts

From **Help → Keyboard Shortcuts…** and the bound `QAction`s:

| Shortcut | Action |
|----------|--------|
| Ctrl+O | Open Binary |
| Ctrl+Shift+O | Open Project |
| Ctrl+S | Save decompiled C |
| Ctrl+Shift+S | Save Project |
| Ctrl+Shift+A | Save Project As |
| Ctrl+Q | Quit (`QKeySequence::Quit`) |
| Ctrl+, | Settings (Tools menu) |
| F5 | Run Full Analysis |
| F6 | Stop analysis / batch |
| Ctrl+1 … Ctrl+5 | Centre tabs (Decompiled C, Assembly, IR, CFG, Synced) |
| Ctrl+` | Show Console |
| Ctrl+Shift+` | Show Problems |
| Ctrl+G | Go to address |
| Ctrl+L | Go to line (window shortcut) |
| G | Assembly: go to address (when that panel is focused) |
| F | Assembly: find in disassembly (when focused) |
| Ctrl+F | Decompiled C / Synced: find in source |
| F3 / Shift+F3 | Decompiled C: find next / previous |
| Alt+← / Alt+→ | Synced tab: navigation history |

---

## Command-line companion (`retdec-decompiler`)

```bash
retdec-decompiler --help
```

Typical usage:

```bash
retdec-decompiler input.exe -o output.c
retdec-decompiler module.wasm -o module.wat
retdec-decompiler script.pyc -o script.py
```

`--buildable` is **on by default** (`.h`, `_stubs.c`, `.buildable.c`).
`--no-buildable` or `RETDEC_EMIT_BUILDABLE=0` turns it off.
`--output-lang` for native binaries is `c|python|csharp|java|wat`
(default `c`). There is no dedicated C++ writer.

Optional GGUF refine (no `--model` flag):

```bash
RETDEC_NEURAL_REFINE=1 RETDEC_NEURAL_MODEL=/path/to/model.gguf \
  retdec-decompiler binary.elf -o output.c
```

On Windows staged builds:

```powershell
.\dist\windows\retdec-decompiler.exe --help
```

Full feature lists: [README.md](../README.md).

---

## Windows-specific notes

| Topic | Detail |
|-------|--------|
| **Portable folder** | `dist\windows\` after `windows_native_build.ps1` contains `retdec-gui.exe`, Qt platforms plugins, and MSVC redistributables. CUDA runtime DLLs only if you built with `RETDEC_ENABLE_CUDA_ACCEL=ON` (default OFF). |
| **Debuggable bundle** | `windows_prepare_debuggable_gui.ps1` produces `dist\windows\debuggable\` with PDBs. |
| **Smoke tests** | `.\scripts\Test-RetdecWindows.ps1 -DistDir dist\windows` |
| **VcXsrv / X11** | For GUI from WSL without WSLg, see `scripts/launch_gui_vcxsrv.sh`. |

---

## Troubleshooting (GUI)

| Problem | What to check |
|---------|----------------|
| GUI does not start | Run from `dist\windows` (or install prefix `bin`) so Qt plugins sit next to the executable; on macOS use `RetDec.app` from the tarball, not a Homebrew-prefix copy. |
| “No CUDA” / slow analysis | The decompiler pipeline is CPU. GUI CUDA settings do not drive `cuda_accel`. |
| AI-assisted naming | Tools → AI Assistant, or `RETDEC_NEURAL_REFINE` + `RETDEC_NEURAL_MODEL`. |
| Empty decompilation | Opening a file does not decompile — press F5. Check **Settings → Advanced → Max functions** (0 = all). |
| Crash on open file | Try a smaller sample; raise **Verbosity** under Advanced; on Windows `run_gui_with_procdump.ps1` (see [scripts/README.md](../scripts/README.md)). |

For **build** failures (OpenSSL, LLVM download, Qt, MSVC env), see [BUILD_REFERENCE.md](BUILD_REFERENCE.md#troubleshooting) and [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md).

---

## Documentation map

| Need | Document |
|------|----------|
| Build from source | [BUILD_REFERENCE.md](BUILD_REFERENCE.md) |
| MSVC + Qt | [WINDOWS_NATIVE_BUILD.md](WINDOWS_NATIVE_BUILD.md) |
| All docs + CI table | [docs/README.md](README.md) |
| Architecture / pipeline | [architecture.md](architecture.md) |
| GUI roadmap | [GUI_ROADMAP.md](internal/GUI_ROADMAP.md) |
| Claims / F1 honesty | [CLAIMS.md](CLAIMS.md) |
| Contributing code | [developer_guide.md](developer_guide.md) |
