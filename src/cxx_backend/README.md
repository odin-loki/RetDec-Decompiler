# cxx_backend — unwired

Plan.md `DEAD-01`. Keep this tree for Phase 4 `LLVM-22`. It is **not** a
shipped `--output-lang cpp` writer. At v2.0.22 the CLI **rejects** `cpp`
(`CLI-01`). Native output is **C**; other languages are input-keyed
(`.pyc` → Python, `.wasm` → WAT, …).

`src/CMakeLists.txt` adds this directory and builds `retdec-cxx-backend`.
Nothing in `src/retdec` consumes these sources as a second HLL backend.
The only in-tree consumer is `tests/cxx_backend/`
(`retdec-cxx-backend-tests`).

Do not advertise C++ output until that wiring exists. Specification
extraction still emits C plus `.config.json` detections.

Sources here: `cxx_ast.cpp`, `cxx_emitter.cpp`, `cxx_lifter.cpp`.
