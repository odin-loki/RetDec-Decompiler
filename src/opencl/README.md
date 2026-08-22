# opencl — experimental, unintegrated

This directory is an **experimental** OpenCL backend. It is **not wired**
into the decompiler pipeline, is **not linked** from `src/retdec`, and is
**not a product feature**.

- `src/CMakeLists.txt` does not add this subdirectory. The sources stay
  in place so include paths `retdec/opencl/*` remain valid.
- Do not advertise OpenCL acceleration as a shipped capability.
