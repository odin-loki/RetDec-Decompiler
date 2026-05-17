# MinGW cross-build (Linux/WSL → Windows PE) — deep dive

This guide documents the **tested and verified** procedure for cross-compiling
RetDec from Linux/WSL to produce native Windows PE binaries using
**MinGW-w64**. MSVC is not supported for this path. Toolchain files live
under `cmake/toolchains/`.

For a shorter **preset and path reference** (`build/linux/mingw-w64-release`, `dist/windows/`, scripts), see [BUILD_REFERENCE.md](BUILD_REFERENCE.md).

---

## Why two stages (host + cross)

1. **LLVM** — Cross-compiling LLVM for Windows still requires **TableGen** to
   run on the **host**. RetDec expects a host `llvm-tblgen` built from the
   **same pinned LLVM revision** as the tree (see `cmake/deps.cmake` /
   `deps/llvm/CMakeLists.txt`). Configure auto-detects it from common dirs
   (e.g. `build/linux/`), or you can pass
   `-DRETDEC_LLVM_TABLEGEN=/path/to/llvm-tblgen` explicitly.

2. **OpenSSL** — With `fileformat` enabled, cross to Windows **requires**
   `RETDEC_BUNDLED_OPENSSL=ON`. The build runs `Configure mingw64` in a
   bundled OpenSSL 3.2.6 source tree. This needs **Perl** and **make** on the
   host. The key requirement: pass **only** `--cross-compile-prefix` to
   OpenSSL's configure, **not** explicit `CC`/`AR`/`RANLIB` env vars —
   OpenSSL derives all tool names from the prefix, and mixing both causes it
   to double-prefix the tools (e.g., `x86_64-w64-mingw32-x86_64-w64-mingw32-ar`).

3. **Tests** — The test suite links host gtest. When cross-compiling,
   `-DRETDEC_TESTS=OFF` must be set. The root `CMakeLists.txt` guards
   `add_subdirectory(tests)` under `if(RETDEC_TESTS)`.

---

## Host prerequisites (Debian/Ubuntu / WSL)

```bash
sudo apt install \
    mingw-w64 \
    g++-mingw-w64-x86-64 \
    binutils-mingw-w64-x86-64 \
    ninja-build \
    perl \
    make \
    cmake
```

---

## Directory layout

| Directory | Role |
|-----------|------|
| `build/linux/` | Native Linux build (preset `full-linux-debug`) — produces `llvm-tblgen` and the working Linux decompiler |
| `build/linux/mingw-w64-release/` | Windows cross-build (configured by `wsl_cross_configure.sh`) |
| `install/linux/mingw-w64-release/` | CMake install prefix for the MinGW cross-build |
| `dist/windows/` | Staged Windows PE binaries + MinGW runtime DLLs (populated by `wsl_cross_build.sh`) |

Keeping both builds from the **same commit** avoids TableGen / IR mismatches.

---

## Step A — Native Linux build (produces llvm-tblgen)

Run from the repo root:

```bash
bash scripts/wsl_configure_nosudo.sh
cmake --build build/linux -j"$(nproc)"
```

Confirm:

```bash
test -x build/linux/deps/install/llvm/bin/llvm-tblgen && echo OK
```

---

## Step B — Windows cross-configure

```bash
bash scripts/wsl_cross_configure.sh
```

What this script does:
- Sets `CMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-mingw-w64.cmake`
- Sets `CMAKE_BUILD_TYPE=Release`, `RETDEC_TESTS=OFF`
- Enables `RETDEC_ENABLE_MACHO_EXTRACTOR=ON`, `RETDEC_ENABLE_UNPACKER=ON`,
  `RETDEC_ENABLE_UNPACKERTOOL=ON` (required by `retdec-decompiler`)
- Injects the host `llvm-tblgen` path via
  `-DRETDEC_LLVM_TABLEGEN=/path/to/retdec-master/build/linux/deps/install/llvm/bin/llvm-tblgen`
- Configures into `build/linux/mingw-w64-release/` (install prefix `install/linux/mingw-w64-release/`)

Manual equivalent:

```bash
ROOT=/path/to/retdec-master
TBLGEN="${ROOT}/build/linux/deps/install/llvm/bin/llvm-tblgen"
cmake -S "${ROOT}" \
      -B "${ROOT}/build/linux/mingw-w64-release" \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="${ROOT}/cmake/toolchains/windows-mingw-w64.cmake" \
      -DCMAKE_INSTALL_PREFIX="${ROOT}/install/linux/mingw-w64-release" \
      -DRETDEC_TESTS=OFF \
      -DRETDEC_ENABLE_GOOGLETEST=OFF \
      -DRETDEC_ENABLE_MACHO_EXTRACTOR=ON \
      -DRETDEC_ENABLE_UNPACKER=ON \
      -DRETDEC_ENABLE_UNPACKERTOOL=ON \
      -DRETDEC_LLVM_TABLEGEN="$TBLGEN"
```

---

## Step C — Build

```bash
cmake --build build/linux/mingw-w64-release -j"$(nproc)"
```

This builds LLVM, Capstone, OpenSSL, and all RetDec libraries from the Unix
host using the MinGW cross-compiler. **Expect 30–90 minutes** for a first
build; subsequent incremental builds are much faster.

---

## Step D — Stage binaries into dist/windows/

```bash
bash scripts/wsl_cross_build.sh
```

What this script does:
- Finds and copies all `.exe` files from `build/linux/mingw-w64-release/src` to
  `dist/windows/` in the repo root
- Copies MinGW runtime DLLs (`libstdc++-6.dll`, `libgcc_s_seh-1.dll`,
  `libwinpthread-1.dll`) from `/usr/lib/gcc/x86_64-w64-mingw32/*/`
- Copies `share/retdec/` support data (signatures, yara rules, etc.) if
  available

After running, `dist/windows/` contains a self-contained Windows deployment:

```
dist/windows/
  retdec-decompiler.exe
  retdec-unpacker.exe
  retdec-qwen3-runner.exe
  libstdc++-6.dll
  libgcc_s_seh-1.dll
  libwinpthread-1.dll
```

---

## Step E — Test on Windows

Copy the `dist/windows/` folder to a Windows machine (or use it directly from
the repo root on Windows since `dist/windows/` is under the Windows-mounted
NTFS path).

**PowerShell smoke tests:**

```powershell
.\scripts\Test-RetdecWindows.ps1
```

The script:
1. Verifies `retdec-decompiler.exe --help` exits successfully
2. Decompiles a Lua 5.4 bytecode sample (if present in `tests/decompile_samples/`)
3. Decompiles a Python `.pyc` sample (if present)
4. Decompiles a Java `.class` sample (if present)
5. Reports `N passed, N failed`

**Manual test:**

```powershell
.\dist\windows\retdec-decompiler.exe --help
.\dist\windows\retdec-decompiler.exe path\to\file.luac -o output.lua
.\dist\windows\retdec-decompiler.exe path\to\file.pyc  -o output.py
.\dist\windows\retdec-decompiler.exe path\to\file.class -o output.java
```

---

## CMake knobs reference

| Variable | Meaning | Notes |
|----------|---------|-------|
| `CMAKE_TOOLCHAIN_FILE` | `cmake/toolchains/windows-mingw-w64.cmake` | Required |
| `RETDEC_LLVM_TABLEGEN` | Absolute path to host `llvm-tblgen` | Auto-detected from common dirs |
| `RETDEC_TESTS` | `OFF` for cross-compile | Tests link host gtest |
| `RETDEC_ENABLE_GOOGLETEST` | `OFF` for cross-compile | |
| `RETDEC_BUNDLED_OPENSSL` | `ON` when cross-compiling with fileformat | CMake errors if OFF |
| `RETDEC_ENABLE_MACHO_EXTRACTOR` | `ON` | Required by decompiler link |
| `RETDEC_ENABLE_UNPACKER` | `ON` | Required by decompiler link |
| `RETDEC_ENABLE_UNPACKERTOOL` | `ON` | Required by decompiler link |

---

## OpenSSL cross-compile — critical notes

### Problem: doubled cross-compile prefix

OpenSSL's `Configure` script uses `--cross-compile-prefix` to **prepend** a
prefix to all tool names (`CC`, `AR`, `RANLIB`, etc.). If you **also** set
these in the environment (e.g. `CC=x86_64-w64-mingw32-gcc`), OpenSSL applies
the prefix again, creating:

```
x86_64-w64-mingw32-x86_64-w64-mingw32-gcc   # WRONG
```

**Fix (in `deps/openssl/CMakeLists.txt`):** When `CMAKE_CROSSCOMPILING` is
true, clear all environment tool variables and rely **solely** on
`--cross-compile-prefix`:

```cmake
if(CMAKE_CROSSCOMPILING)
    set(_OSSL_CONFIGURE_ENV "")   # no CC/AR/RANLIB env vars
    set(_OSSL_CROSS "--cross-compile-prefix=x86_64-w64-mingw32-")
endif()
```

### Problem: libraries installed to lib64/ not lib/

Some OpenSSL versions install to `lib64/` by default. Add `--libdir=lib` to
the OpenSSL `CONFIGURE_COMMAND`:

```cmake
... Configure mingw64 ${_OSSL_CROSS} --prefix=... --libdir=lib no-shared no-tests no-asm
```

---

## LLVM TableGen — discovery and manual override

`cmake/retdec_cross_llvm_tblgen.cmake` searches these directories for a host
`llvm-tblgen` binary (relative to the build or repo root):

- `build/linux/deps/install/llvm/bin/llvm-tblgen`
- Other common in-repo dirs under `build/` (see `cmake/retdec_cross_llvm_tblgen.cmake`)

If not found automatically:

```bash
# Option 1: pass on the command line
cmake ... -DRETDEC_LLVM_TABLEGEN=/path/to/retdec-master/build/linux/deps/install/llvm/bin/llvm-tblgen

# Option 2: set in environment before cmake
export RETDEC_LLVM_TABLEGEN=/path/to/retdec-master/build/linux/deps/install/llvm/bin/llvm-tblgen
cmake ...
```

---

## test_harness.cpp — Windows API

`src/testing/test_harness.cpp` uses `MAX_PATH`, `GetTempPathA`,
`GetTempFileNameA`, and `DeleteFileA` under `#ifdef _WIN32`. These require
`<windows.h>` which must be included explicitly:

```cpp
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif
```

This is already applied in the source tree.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| CMake warns: no `llvm-tblgen` | Host build missing or at wrong path | Run `wsl_configure_nosudo.sh` first; or pass `-DRETDEC_LLVM_TABLEGEN=...` |
| `x86_64-w64-mingw32-x86_64-w64-mingw32-ar: No such file` | OpenSSL doubling prefix | Use only `--cross-compile-prefix`, no env CC/AR vars |
| OpenSSL SHA256 mismatch | Wrong tarball URL or version | Verify URL and hash in `deps/openssl/CMakeLists.txt` (currently OpenSSL 3.2.6) |
| OpenSSL `libcrypto.a` not found, libs in `lib64/` | Default install dir | Add `--libdir=lib` to OpenSSL configure; or `ln -sfn lib64 lib` |
| `retdec::macho-extractor` target not found | Component disabled | Add `-DRETDEC_ENABLE_MACHO_EXTRACTOR=ON` |
| gtest target not found | Tests included in cross build | Set `-DRETDEC_TESTS=OFF -DRETDEC_ENABLE_GOOGLETEST=OFF` |
| LLVM `NATIVE` subdirectory uses cross-compiler | Wrong tblgen | Point `RETDEC_LLVM_TABLEGEN` to the **host** binary |
| `cmake --install` fails on missing `CHANGELOG.md` / `LICENSE` | Install step checks for them | Both files exist in the repo root; the `wsl_cross_build.sh` script bypasses install and directly copies `.exe`/`.dll` instead |
| `Test-RetdecWindows.ps1` stops on stderr | `$ErrorActionPreference = "Stop"` | Script uses `"Continue"` — decompiler prints info to stderr normally |

---

## CUDA acceleration

The Windows cross-compiled build does **not** include CUDA support. NVCC
cannot cross-compile GPU kernels. The Windows build automatically uses the
CPU-only fallback for all analysis passes. For GPU-accelerated decompilation,
use the native Linux/WSL build.

---

## Related files

- `cmake/toolchains/windows-mingw-w64.cmake` — MinGW-w64 GCC toolchain file
- `scripts/wsl_cross_configure.sh` — configures the Windows cross-build
- `scripts/wsl_cross_build.sh` — builds and stages Windows binaries
- `scripts/Test-RetdecWindows.ps1` — PowerShell smoke test suite
- `scripts/wsl_configure_nosudo.sh` — configures the native Linux build
- `scripts/wsl_build.sh` — builds the native Linux target
- `deps/openssl/CMakeLists.txt` — OpenSSL bundled external project
- `deps/llvm/CMakeLists.txt` — LLVM external project
- `docs/README.md` — developer quick reference
