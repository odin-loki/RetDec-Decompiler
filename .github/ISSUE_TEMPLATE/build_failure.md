---
name: Build failure
about: CMake, compiler, or packaging errors
title: "[build] "
labels: build
assignees: ''
---

## Summary

<!-- e.g. configure fails on full-linux-debug with Qt6 not found -->

## Environment

- OS / distro (Linux / Windows / macOS + arch):
- Compiler (GCC/Clang/AppleClang/MSVC + version):
- CMake version (3.26+ required by `CMakePresets.json`):
- Preset or command used:
  - Linux / WSL / **macOS**: `full-linux-debug` or `full-linux-release` (`build/linux`)
  - Windows MSVC: `full-windows-debug` or `full-windows-release` (`build/windows`)
  - Core-only: `core-debug` / `core-debug-msvc`
- Qt 6 installed? (`full-*` presets **require** Qt 6; `RETDEC_REQUIRE_QT6=ON`)

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
