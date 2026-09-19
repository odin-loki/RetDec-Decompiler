# opencl — experimental, unintegrated

This directory is a **parked research** OpenCL backend. It is **not wired**
into the decompiler pipeline, is **not linked** from `src/retdec`, and is
**not a product feature** at v2.0.22. `C-CUDA-PIPE` is withdrawn.

- `src/CMakeLists.txt` does **not** add this subdirectory. The sources stay
  in place so include paths `retdec/opencl/*` remain valid.
- `src/opencl/CMakeLists.txt` calls `find_package(OpenCL REQUIRED)` — that
  is why the dir is not on the default product configure.
- Do not advertise OpenCL acceleration as a shipped capability.

v2.0.22 installers (Linux, macOS, Windows) and CMake presets
(`full-linux-*`, `full-windows-*`) do not build or ship this library.

Public write-up: [docs/CUDA_CAPABILITIES.md](../../docs/CUDA_CAPABILITIES.md).
[docs/RESEARCH_FRONTIERS.md](../../docs/RESEARCH_FRONTIERS.md) § GPU
acceleration (parked).
