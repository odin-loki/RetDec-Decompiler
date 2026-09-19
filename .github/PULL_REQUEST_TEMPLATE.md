## Summary

<!-- What does this PR change and why? RetDec Imortek is a specification-extraction decompiler (v2.0.24): buildable C default, input-keyed outputs, Qt 6 GUI, optional llama.cpp. -->

## Type of change

- [ ] Bug fix
- [ ] New feature
- [ ] Documentation
- [ ] Refactor / cleanup
- [ ] Build / CI

## Testing

Build with a current CMake preset, not an ad-hoc cache:

- Linux / WSL / **macOS**: `cmake --preset full-linux-debug` then `cmake --build --preset full-linux-debug` (`build/linux`)
- Windows MSVC: `full-windows-debug` (`build/windows`)
- Core-only: `core-debug` / `core-debug-msvc`

```bash
# example
ctest --preset full-linux-debug
# or: ctest --test-dir build/linux --output-on-failure
```

- [ ] `bash scripts/check_format.sh`
- [ ] Tests pass locally (preset above)
- [ ] GUI tested if applicable (`retdec-gui`, macOS `RetDec.app`, or `--headless-decompile`)

CUDA/OpenCL accel is parked (`RETDEC_ENABLE_CUDA_ACCEL=OFF`). Do not add a CUDA-on job unless the PR is explicitly about that research tree. Do not disable, SKIP, or loosen tests.

## Documentation

- [ ] Updated docs (architecture / user manual / BUILD_REFERENCE) if behaviour changed
- [ ] No doc update needed

## Licence

By submitting this pull request, I confirm that I have read [CLA.md](../CLA.md)
and that my contribution is licensed under those terms (outbound relicensing
to Imortek for AGPL-3.0+ and commercial).
