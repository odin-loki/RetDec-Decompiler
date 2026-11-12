---
name: Build failure
about: CMake, compiler, or packaging errors
title: "[build] "
labels: build
assignees: ''
---

## Summary

<!-- e.g. configure fails on full-linux-debug: compiler does not support C++23 -->

## Environment

- OS / distro (Linux / Windows / macOS + arch):
- Compiler (GCC/Clang/AppleClang/MSVC + version):
- CMake version (3.26+ required by `CMakePresets.json`):
- Preset or command used:
  - Linux / WSL / **macOS**: `full-linux-debug` or `full-linux-release` (`build/linux`)
  - Windows MSVC: `full-windows-debug` or `full-windows-release` (`build/windows`)
  - Core-only: `core-debug` / `core-debug-msvc`
- Compiler and version (C++23 required: GCC 13+, Clang 17+, Apple Clang 15+, MSVC 2022 17.6+). The GUI needs no toolkit (FuseUI is vendored); `-DRETDEC_GUI=OFF` builds CLI-only.

CUDA / OpenCL are **not** required. `RETDEC_ENABLE_CUDA_ACCEL` defaults OFF on every full preset. Do not enable them to “fix” a product build. llama.cpp (`RETDEC_ENABLE_LLAMACPP`) is optional.

## Configure / build log

<details>
<summary>Log excerpt</summary>

```
Paste the failing command and last ~50 lines of output.
```

</details>

## What you tried

<!-- Optional: clean build dir, different preset, `scripts/wsl_configure_nosudo.sh`, `scripts/windows_native_configure.ps1`, `scripts/doctor.sh` / `scripts/doctor.ps1`. -->

## Additional context

<!-- Link to docs/BUILD_REFERENCE.md if relevant. Packaging: Linux/macOS/Windows installers are `.github/workflows/release-installers.yml`. -->
