---
name: Bug report
about: Report incorrect decompilation, crashes, or GUI issues
title: "[bug] "
labels: bug
assignees: ''
---

## Summary

<!-- One paragraph: what went wrong? -->

## Environment

- OS (Linux distro / Windows / macOS version + arch):
- RetDec version or commit (e.g. 2.0.22, or `git rev-parse --short HEAD`):
- How installed: GitHub Release installer / portable zip / source build
- CMake preset if built from source (`full-linux-debug`, `full-linux-release`, `full-windows-debug`, `full-windows-release`, `core-debug`, `core-debug-msvc`, …). `full-linux-*` is also the macOS preset (`build/linux`).
- CLI or Qt 6 GUI (`retdec-gui` / macOS `RetDec.app`)
- Input file type (PE / ELF / Mach-O / .pyc / .wasm / …)

Do **not** fill CUDA on/off: GPU acceleration is parked and not a product path (`RETDEC_ENABLE_CUDA_ACCEL` defaults OFF). This project does not use the OSS-Fuzz DecompileBench paper corpus or the missing `retdec/retdec:v5.0` image.

## Steps to reproduce

1.
2.
3.

## Expected behaviour


## Actual behaviour


## Logs / screenshots

<!-- Attach decompiler log, a redacted `.config.json` snippet, or screenshot. Do not attach customer binaries you cannot share. -->

## Additional context

<!-- Optional: minimal sample binary, related issue, workaround tried. -->
