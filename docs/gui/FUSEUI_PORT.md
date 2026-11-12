# retdec-gui on FuseUI and Forge

retdec-gui no longer uses Qt. It is built on **FuseUI** (the GUI toolkit from
[FUSE](https://github.com/odin-loki/FUSE), vendored in `third_party/fuseui`)
and **Forge** (vendored in `third_party/forge`) for everything that is not
drawing: processes, files, JSON, settings, hashing, threads.

The Qt implementation is kept, read-only, in `.qt-gui-reference/` while the
port is in progress. It is the specification: every panel, menu action,
shortcut and behaviour it has, the new GUI has. It is deleted when the port is
complete. Nothing builds from it.

## Layout

| Directory | Target | Rule |
|-----------|--------|------|
| `include/retdec/gui/model/`, `src/gui/model/` | `retdec-gui-model` | No UI. C++23 + Forge only. Everything a panel computes that is not drawing lives here (filtering, sorting, graph layout, diffing, log parsing, artifact loading) so it is unit-testable. Namespace `retdec::gui::model`. |
| `include/retdec/gui/ui/`, `src/gui/ui/` | `retdec-gui-ui` | FuseUI views. Namespace `retdec::gui::ui`. |
| `src/gui/app/` | `retdec-gui` | `main.cpp`: window, headless mode, command line. |
| `src/gui/resources/` | installed to `share/retdec/gui` | Themes (FuseUI CSS), LLVM pass presets, fonts. Found at run time beside the binary (`<bin>/../share/retdec/gui`), never compiled in. |
| `tests/gui/` | `retdec-gui-tests` | gtest, one `<name>_test.cpp` per unit; globbed. |

All three source directories are globbed by `src/gui/CMakeLists.txt`: adding a
file needs no CMake edit.

## Build and test without the rest of RetDec

retdec-gui talks to the decompiler only as a child process, so it builds on its
own in a couple of minutes:

```sh
cmake -S cmake/gui_standalone -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build/gui && ./build/gui/gui-tests/retdec-gui-tests
```

(needs `libgtest-dev`). The full RetDec build includes the same targets.

## Conventions

- **C++23.** `std::expected<T, std::string>` for fallible calls, `std::format`,
  `std::string` (UTF-8) for text, `std::filesystem::path` for paths. No
  exceptions across the model API.
- **Forge, not hand-rolled code**, for: processes (`forge/os/process.hpp`:
  `spawn_spec`, `child`), files (`forge/os/fs.hpp`), settings location
  (`forge/os/settings.hpp`), clipboard (`forge/os/clipboard.hpp`), JSON
  (`forge/json/json.hpp`), hashing (`forge/hash/blake3.hpp`), thread pool
  (`forge/task/pool.hpp`), dynamic libraries (`forge/os/dynlib.hpp`). Read the
  header before using it; Forge's API is its own, not std's.
- **FuseUI** headers: `fuse/ui/ui.hpp` (UiContext, NodeHandle, signals, CSS),
  `fuse/ui/widgets/widgets.hpp` (every widget), `fuse/app/app.hpp`
  (Application, AppWindow, Settings, CommandLine). `docs/ui/README.md` in the
  FUSE tree and `third_party/fuseui/Source/FUSE/UI/App/samples/*.cpp` show use.
  One owner thread: other threads hand work back with `ui.post(...)`.
- **A panel** is a class owning a subtree:

  ```cpp
  namespace retdec::gui::ui {
  class FunctionListPanel {
  public:
      FunctionListPanel(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
      fuse::ui::NodeHandle root() const;
      void setFunctions(std::vector<model::FunctionEntry> fns);      // data in
      std::function<void(uint64_t address, const std::string& name)>
          onFunctionActivated;                                        // events out
      // Test hooks: read back what is shown, drive it like a user would.
  };
  }
  ```

  The main window owns panels (`std::unique_ptr`), places their roots in docks
  and tabs, and wires their callbacks. Panels never reach into each other.
- **Tests** cover the model directly and drive views through a headless
  `fuse::ui::UiContext` (bundled font: `fuse::text::Font::bundled()`), using
  the same input calls a user's events produce (`pointerDown`, `keyDown`,
  `textInput`) or the widget setters, then `ui.update(dt)` and assertions on
  node text/values. Port the Qt tests in `.qt-gui-reference/tests/` for the
  same behaviour; they are the acceptance criteria.

## Headless and CI

`RETDEC_GUI_HEADLESS=1` or `--headless` runs without an OS window (the
AppWindow presents to nothing). `--headless-decompile <binary>` launches the
decompiler exactly as the CLI would, waits, loads the artefacts and exits with
the decompiler's status; `gui_headless_decompile_fib` and
`gui_cli_decompile_parity_fib` in `tests/decompiler` depend on that contract.
