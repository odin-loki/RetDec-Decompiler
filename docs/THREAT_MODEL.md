# Threat model (audit S11)

## Asset and adversary

The **analyst host** (workstation or CI runner that executes RetDec) is the
asset. The **input file is fully attacker-controlled**: PE/ELF/Mach-O,
managed bytecode, archives, optional GGUF weights, and any strings those
files contain. Decompiled C is data, not a trusted program. Compromise of
the analyst account, files, or credentials is in-scope impact.

This document states what the tree does today. It does not claim a sandbox
that is not implemented.

## Trust boundaries

| Boundary | Code | Who crosses it | Sandboxed? |
|----------|------|----------------|------------|
| File parse | `src/fileformat`, `src/fileinfo`, managed parsers | Hostile headers, sections, relocs | **No.** In-process. Fuzz targets exist under `tests/managed_integration/fuzz/` (ELF, PE, Mach-O, WASM, DEX, JVM, PYC); they are not a runtime sandbox. |
| YARA | `src/yaracpp`; `src/fileinfo/pattern_detector`; `src/bin2llvmir` provider init | Rule files plus the input binary | **No.** libyara runs in the decompiler process. |
| Unpacker emulation | `src/unpacker`, `src/unpackertool` (UPX/mpress stubs, NRV, unfilter) | Packed payload and decompressor | **No.** Emulation is in-process as the analyst user. |
| Neural prompt | `src/neural/prompts.cpp` | Decompiled text (may contain binary-lifted strings) | **No OS sandbox.** Prompts run `stripCStringLiterals` before the model sees function source (`tests/neural/mock_test.cpp`). |
| GUI subprocess | `src/gui` `QProcess` → `retdec-decompiler` | Same untrusted input as CLI | **No.** Out-of-process only. No seccomp, AppContainer, or `sandbox_init`. Child inherits host rights. |
| GPU kernel | `src/cuda_accel`, `src/opencl`; optional llama.cpp CUDA | Host buffers / model weights if those builds are enabled | **No.** `cuda_accel` / `opencl` are unintegrated and default **OFF**. llama.cpp GPU offload is a separate opt-in (`RETDEC_NEURAL_GPU_OFFLOAD`). Kernels are not isolated from the host. |

`SECURITY.md` tells operators to run untrusted binaries in an isolated VM
or container. That isolation is **operator-provided**, not built into
RetDec.

## Neural (N3, N5, N6)

- **Decompiled C is never executed** on the host.
- The compile-gate (`src/neural/gates.cpp`) spawns argv
  `{cc, "-fsyntax-only", "-w", src}` (Windows: `CreateProcess` of
  `gcc -fsyntax-only -w`). It does not link or run the translation unit.
  `RETDEC_NEURAL_GATE_CC` selects the compiler binary; there is no
  `std::system` / shell concatenation.
- If `RETDEC_NEURAL_DIFF_GATE` is set, the gate **warns and skips**.
  Runtime differential execution is treated as pass-without-run.
- **N6 model allowlist is still default-off.** There is no shipped
  `support/models.json`. SHA-256 is checked only when
  `RETDEC_NEURAL_MODEL_SHA256` is set, or when the filename matches the
  pinned Qwen 3.5 Q4_K_M hint. Other GGUF paths load without a pin
  (`tests/neural/mock_test.cpp` `UnpinnedOtherGgufPassesWithoutEnv`).
- Multimodal `mmproj` / `-VL-` names are rejected.
- Prompt construction strips C `"..."` and `'...'` literals so binary
  strings cannot inject instructions into the prompt.

Neural refinement itself is opt-in (`RETDEC_NEURAL_REFINE` + model path;
llama.cpp is off in default installers).

## What is not a security boundary

- Correctness of decompiled C, types, or semantic detections.
- Plugins (`IDecompilerPlugin` and friends) — loaded code runs as the GUI
  user.
- Optional `RETDEC_NO_NETWORK=1` / `RETDEC_NEURAL_OFFLINE_ONLY` — policy
  flags, not a verified syscall filter (audit S17 is open).

## Residual risk (honest)

Parsers, YARA, and unpacker emulation share the analyst process. A
malicious sample can attempt memory corruption, huge allocations, or
hostile rule/data interaction. The GUI child can write wherever the user
can. Until S15 (sandboxed worker) and N6 (default-on allowlist) exist,
treat RetDec as an unsandboxed native analysis tool.
