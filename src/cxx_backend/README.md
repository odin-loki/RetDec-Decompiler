# cxx_backend — unwired

Plan.md `DEAD-01`. Keep this tree for Phase 4 `LLVM-22`. It is **not**
a shipped `--output-lang cpp` writer (`CLI-01` rejects `cpp`).

Nothing in `src/retdec` consumes these sources as a second HLL backend.
Do not advertise C++ output until that wiring exists.
