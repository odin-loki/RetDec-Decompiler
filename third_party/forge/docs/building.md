# Building

## Concepts
- FORGE builds with CMake. Most layers are header-only INTERFACE targets; a few are static libraries, plus the C ABI shared library and the two apps.
- Code generators in `ops/gen/` run at configure and build time (Python 3). They produce the op table, x86 intrinsic wrappers and the GPU kernels.
- GPU runtimes are not linked. They are loaded with `dlopen`/`LoadLibrary` at run time, so one binary runs with or without a GPU driver. What the build needs is a way to compile the kernels (see Requirements).
- Configure messages say what was found, for example `FORGE: Vulkan backend disabled (need glslangValidator + vulkan/vulkan.h)` or `FORGE: CUDA/HIP images: ...`.

## Requirements
- CMake **3.25+**. The presets use Ninja; `ios` uses Xcode.
- A **C++23** compiler and a C compiler. CI builds with g++-14, clang-18, AppleClang (macos-latest), MSVC (windows-latest), MinGW-w64 GCC (MSYS2 UCRT64) and FreeBSD 14.3's base clang. Clang older than 19 and libc++ older than 20 get configure-time workarounds (`std::expected`, `-fexperimental-library`).
- **Python 3** at configure time (required).
- Optional, each enabling more:

| Tool | Enables |
|------|---------|
| `glslangValidator` + `vulkan/vulkan.h` | SPIR-V kernels embedded in the binary, and the Vulkan backend. `spirv-val` also validates them |
| `clang` with the NVPTX / AMDGPU backends (`clang-19`, `clang-18`, `clang-20` or `clang`; the host compiler's own clang when the host compiler is clang) | Embedded CUDA PTX (`sm_70`) and HIP code objects. Override with `-DFORGE_GPU_CLANG=...` |
| `ld.lld` / `lld`, or `zig` (also found through the `ziglang` Python package) | Linking the HIP code objects. Override with `-DFORGE_LLD=...` |
| `llvm-spirv` | SPIR-V for Level Zero |
| OpenSSL | TLS for `forge rpc-serve` (`FORGE_TLS`, on by default when OpenSSL is found). Pass `-DOPENSSL_ROOT_DIR=<prefix>` if needed |
| `nvcuvid.h` (NVIDIA Video Codec SDK) | NVDEC in `forge_video` (`FORGE_NVCUVID_INCLUDE`) |
| Python `numpy`, `safetensors` | Golden-model tests and the tests that use the golden model |
| Python `onnx`, `cryptography` | `onnx_import`, `modelfile_signing` |

Without a CUDA/HIP clang the CUDA/HIP runtimes have no kernels; the tests that need them skip. OpenCL C and Metal source are always generated and compiled by the driver at run time. CUDA/HIP images are not built for Android or iOS.

## Host build

```sh
cmake --preset dev                 # build/dev, RelWithDebInfo
cmake --build build/dev -j4
ctest --test-dir build/dev -j4 -LE "slow|emu"
```

Other host presets: `release` (`build/release`, Release) and `asan` (`build/asan`, Debug with ASan + UBSan). `cmake --workflow --preset release` runs configure, build, test (excluding label `slow`) and package (TGZ) in one go.

## Presets

| Preset | Build dir | Toolchain | Status |
|--------|-----------|-----------|--------|
| `dev`, `release`, `asan` | `build/dev`, `build/release`, `build/asan` | host | `dev` is used by the Linux and macOS CI jobs |
| `windows-mingw` | `build/win64` | `cmake/toolchains/mingw-w64.cmake` (Debian `x86_64-w64-mingw32-*-posix`) | Cross build; tests run under wine |
| `android-arm64` | `build/android-arm64` | `cmake/toolchains/android-arm64.cmake` (`ANDROID_NDK_HOME`), sets `FORGE_JNI=ON` | CI cross-builds with the same toolchain file |
| `macos-osxcross` | `build/macos` | `cmake/toolchains/macos-osxcross.cmake` (`OSXCROSS_ROOT`, `OSXCROSS_HOST`) | **Not run.** Native macOS builds use `dev` |
| `ios` | `build/ios` | `cmake/toolchains/ios.cmake`, Xcode generator | CI builds `forge_c` only; nothing has run on iOS |

Build presets exist for every configure preset except `ios`. Test presets: `dev`, and `release` (excludes label `slow`). Package and workflow presets: `release`.

## CMake options

| Option | Default | Meaning |
|--------|---------|---------|
| `FORGE_BUILD_TESTS` | ON | Unit, script, fuzz and doc-example tests |
| `FORGE_BUILD_APPS` | ON | `forge` and `forge-serve` (the golden-model tests need them) |
| `FORGE_BUILD_EXAMPLES` | ON | `examples/integrations/*.cpp` as `example_*` |
| `FORGE_SANITIZE` | OFF | ASan + UBSan |
| `FORGE_TSAN` | OFF | ThreadSanitizer. Cannot be combined with `FORGE_SANITIZE` |
| `FORGE_REPRODUCIBLE` | ON | Map build paths out of artefacts (`-ffile-prefix-map`) and drop the build id, for bit-identical rebuilds. No prefix map with MSVC; no `--build-id=none` on Apple or MSVC |
| `FORGE_HARDENING` | ON | Hardening flags, each probed and skipped if the toolchain rejects it (FSS-BLD-1, `cmake/forge_hardening.cmake`). GCC/Clang: `-fstack-protector-strong`, `-fstack-clash-protection`, `-D_GLIBCXX_ASSERTIONS` (libstdc++) or libc++ >= 18 hardening mode `fast` (not Apple/NDK), `-fcf-protection=full` (x86_64) or `-mbranch-protection=standard` (aarch64, not Apple: Darwin arm64 does not unwind PAC-signed frames); `-D_FORTIFY_SOURCE=3` on Linux (not Android) in optimised configs, not with `FORGE_SANITIZE`/`FORGE_TSAN`; ELF: PIE (`CMAKE_POSITION_INDEPENDENT_CODE`, `-pie`), `-Wl,-z,relro,-z,now,-z,noexecstack`; MinGW: `--dynamicbase --nxcompat --high-entropy-va`. MSVC: `/GS /guard:cf`, link `/guard:cf /CETCOMPAT` (x64) `/DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA`; no `/sdl` and no `/Qspectre` (security-standard.md BLD-1). clang-cl gets none. What was enabled: `forge::build_hardening()` in `core/include/forge/core/build_info.hpp`. Cost (2026-11-05, MinGW gcc, ON vs OFF, best of 7-15 interleaved runs on a shared machine, noise about 5%): `forge bench --gemm 256` avx2/avx512 within 1%, tiny-model prefill/decode within 0.5%. Only the stack protector changes the GEMM kernels' code (a canary per call) |
| `FORGE_TLS` | ON | Use OpenSSL for `rpc-serve` TLS when found |
| `FORGE_JNI` | OFF | Build `libforge_jni` for the Kotlin binding |
| `FORGE_LIBFUZZER` | OFF | Link fuzz targets with libFuzzer (clang) instead of the built-in driver; they are then not ctest tests |
| `FORGE_FUZZ_ITERS` | 4000 | Mutated inputs per fuzz target under ctest |
| `FORGE_HIP_ARCHS` | `gfx90a gfx942 gfx1100` | AMDGPU targets embedded for HIP |
| `FORGE_LOG_MIN_LEVEL` | `none` | Compile-time log strip: `trace`, `debug`, `info`, `warn`, `error`, `fatal`, `off` or `none`. Adds `FORGE_LOG_MIN_LEVEL=<0..6>` to every target linking `forge_core`, so log calls below that level compile to nothing. Any other value is a configure error. With it set, `test_log` reports skipped |
| `FORGE_FETCH_UCD` | OFF | Download the Unicode conformance files for `test_text_conformance` (otherwise it skips) |
| `FORGE_NVCUVID_INCLUDE` | empty | Directory with `nvcuvid.h` / `cuviddec.h` |
| `FORGE_GOLDEN_PYTHON` | the Python CMake found | Interpreter with torch/transformers/gguf for `golden_tiny_qwen3_5` and `golden_quant_inplace` |
| `FORGE_TEST_SHELL_PATH` | `../opt/shells/bin` if it exists | Extra `PATH` entries for the shell backend tests |
| `FORGE_IOS_SIMULATOR` | OFF | With `ios.cmake`: build for the simulator (arm64 + x86_64) |

## Targets
Header-only INTERFACE targets: `forge_core`, `forge_os`, `forge_ops`, `forge_simd`, `forge_verify`, `forge_gpu`, `forge_graph`, `forge_modelfile`, `forge_inference`, `forge_loadbalancer` (and `forge_rpc_tls` when OpenSSL is found). Compiled targets:

| Target | Output |
|--------|--------|
| `forge_simd_mv` | Static library with every compiled kernel variant. Link it whenever you use simd, gpu, graph or inference |
| `forge_trust` | Static library: vendored Monocypher 4.0.2 |
| `forge_text` | Static library: `forge::os::text` and `forge::os::locale` |
| `forge_mobile` | Static library: `forge::os::mobile` |
| `forge_script` | Static library: the script runtime used by `forge script` |
| `forge_video` | Static library: video decode support |
| `forge_metal` | Static library, Apple only (`gpu/src/metal.mm`) |
| `forge_c` | The C ABI: `libforge.so` (soname `libforge.so.1`, version 0.1.0), `forge.dll` on Windows |
| `forge_jni` | `libforge_jni` (with `FORGE_JNI=ON`) |
| `forge_script_example_plugin` | `hello_plugin`, an example native script plugin (also used by `test_script`) |
| `forge`, `forge-serve` | The apps |

Generated files go to `build/<dir>/generated/`: the op table (`forge/ops/ops.gen.hpp`, `ops.md`), x86 intrinsic wrappers (`forge/simd/raw/x86.gen.hpp`), GPU kernels (`gpu/*.comp`, `kernels.cu`, `kernels.cl`, `kernels.metal`), the embedded SPIR-V and CUDA/HIP images (`forge/gpu/spirv.gen.hpp`, `forge/gpu/gpu_images.gen.hpp`) and the shell backend registry.

## Installing and packaging
`cmake --install build/release --prefix /opt/forge` installs `libforge`, `forge_simd_mv`, `forge_trust`, the apps, the licence files, `README.md`, `SECURITY.md` and headers. CPack (`cpack` in the build directory, or `tools/release.py build`) produces binary and source archives: TGZ (plus DEB on Linux), or ZIP on Windows.

**Known gap:** the install rule copies headers from `core`, `simd`, `ops`, `gpu`, `graph`, `modelfile`, `inference`, `support` and `verify`, but not from `os`, `loadbalancer` or `script`, nor the generated headers, and it does not install `forge_text`, `forge_mobile` or `forge_script`. `core` includes `<forge/os/os.hpp>`, so an installed tree cannot yet be used to compile against the C++ headers. Build against the source tree, or use the C ABI. Installed binaries look for import templates in the source tree they were built from unless `FORGE_TEMPLATES` is set.

## Windows

Native MSVC (as in the `windows-msvc` CI job), from a Visual Studio developer prompt:
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl
cmake --build build -j4
ctest --test-dir build -LE "slow|emu" -j2
```
The build sets `/utf-8`, `/EHsc`, `/bigobj`, `/O2` for optimised configs, and an 8 MiB main-thread stack.

Native MinGW (as in the `windows-mingw` CI job): in an MSYS2 UCRT64 shell with `mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,python}`, run the same commands without the compiler options.

Cross build from Linux, run under wine:
```sh
cmake --preset windows-mingw && cmake --build build/win64 -j4
tests/wine_run.sh build/win64      # runs every test_*/example_*/fuzz_* .exe, prints PASS/SKIP/FAIL
```
`wine_run.sh` uses `WINEPREFIX` (default `~/.wine-forge64`) and creates it on first use. Use a pure 64-bit prefix; a wow64 prefix fails with `kernel32.dll c0000135`.

The mock GPU driver tests point `LD_LIBRARY_PATH` at the mock libraries. On Windows the CUDA and HIP mock tests skip; [STATUS.md](STATUS.md) lists which mock tests pass on each CI job.

## macOS and iOS
- Native macOS: `cmake --preset dev` with AppleClang, as in the `macos` CI job. Metal and NEON are picked up automatically.
- iOS: `cmake -S . -B build/ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios.cmake` (deployment target 16.3). CI builds only `forge_c`, with `FORGE_BUILD_TESTS=OFF FORGE_BUILD_APPS=OFF FORGE_BUILD_EXAMPLES=OFF` and `CODE_SIGNING_ALLOWED=NO`.
- `tools/build_xcframework.sh` builds `bindings/apple/FORGE.xcframework` (iOS device, simulator, macOS). **Not run.**

## FreeBSD
Native, as in the `freebsd` CI job (FreeBSD 14.3 VM): `pkg install cmake ninja python3`, then the host build commands with `-DFORGE_BUILD_EXAMPLES=OFF`, and `ctest -LE "slow|emu|gpu"`. `ci/freebsd-vm.sh` (used by `ci/run-local.sh`) instead cross-builds with clang and a FreeBSD sysroot (`ci/freebsd-cross.cmake`) and runs ctest in a QEMU guest.

## Android (NDK)
```sh
export ANDROID_NDK_HOME=/path/to/android-ndk
cmake -S . -B build/android-x86_64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake -DANDROID_ABI=x86_64
cmake --build build/android-x86_64 -j4
ANDROID_HOME=/path/to/sdk tests/android_emulator.sh native build/android-x86_64
```
- The toolchain defaults to `arm64-v8a`, API 26, and needs NDK r26 or newer.
- `tests/mobile_build.sh android <abi>` cross-builds and checks the JNI exports (ctest `mobile_android_<abi>`).
- `android_emulator.sh native` pushes every `test_*`/`example_*` binary, the shared libraries, `libc++_shared.so` and the import templates, runs each with `FORGE_THREADS=2` and `FORGE_TEMPLATES` set, retries when adb drops, and exits 77 when no device is attached. `FORGE_TEST_TIMEOUT` sets the per-test limit (default 600 s). `android_emulator.sh apk` builds and installs the example APK without gradle.
- The `android` CI job cross-builds arm64-v8a and x86_64, then on an API 34 x86_64 emulator runs the `--emit android` goldens with `/system/bin/sh` and the native `test_script` (`.github/android_check.sh`).

## qemu, SDE and cross-link checks
- `cmake/toolchains/aarch64-sve-qemu.cmake` and `riscv64-rvv-qemu.cmake` cross-build and run ctest under qemu-user. Extra `lane_sve_vl<N>` / `lane_rvv_vl<N>` tests run `test_simd` at several vector lengths and check that the scalable table is active; `tile_sme_svl<N>` does the same for SME when the compiler supports it.
- With `zig` on `PATH` (or `ZIG_PATH`), the `xos_*` tests (labels `cross;slow`) compile and link the OS-layer tests for x86_64-freebsd, aarch64-macos, x86_64-macos, x86_64-windows-gnu and aarch64-linux-musl, and compile the script emitter, mobile and text sources for most of those targets. They are compile/link checks only.
- With Intel SDE (`sde64`, or `SDE_PATH`) and a compiler that supports AMX, `tile_amx_sde_{spr,gnr,icl}` run the AMX tile kernels under emulation (label `emu`).

## GPU hardware notes
- CUDA has run on one real GPU: an RTX 3090 on Windows 11 with the CUDA 13.2 toolkit installed, using a mingw-w64 cross build of `test_cuda_hip`. Results are in [STATUS.md](STATUS.md).
- HIP, OpenCL and Level Zero have only run against the mock drivers in `tests/`. Metal's `test_gpu_metal` passes on the GitHub macOS runner's GPU.
- The CI runners have no `glslangValidator`, so Vulkan is compiled out in every CI job. Vulkan has run only on software devices, locally.

## Building with zig as the compiler
When no system compiler is available, `pip install ziglang` and point CMake at wrapper scripts that run `python -m ziglang c++ "$@"` / `cc "$@"` (`CC=... CXX=... cmake --preset dev`). The CUDA/HIP image build looks for a binary named `clang`, so with zig alone there are no CUDA/HIP images and the tests that need them skip.
