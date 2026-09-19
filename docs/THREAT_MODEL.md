# Threat model (audit S11)

## Asset and adversary

The **analyst host** (workstation or CI runner that executes RetDec) is the
asset. The **input file is fully attacker-controlled**: PE/ELF/Mach-O,
managed bytecode, archives, optional GGUF weights, and any strings those
files contain. **Decompiling untrusted binaries is the product**, not an
edge case. Decompiled C is data, not a trusted program. Compromise of
the analyst account, files, or credentials is in-scope impact.

This document states what the tree does today. It does not claim a sandbox
that is not implemented.

## Trust boundaries

| Boundary | Code | Who crosses it | Sandboxed? |
|----------|------|----------------|------------|
| File parse | `src/fileformat`, `src/fileinfo`, managed parsers | Hostile headers, sections, relocs | **No.** In-process. Standalone fuzz (`scripts/standalone_fuzz.sh`) covers bytecode/metadata/PeLib/lattice/demangle/loadersim; ELF/PE/Mach-O fileformat harnesses need `RETDEC_FUZZ=ON` and run in `fuzz-libfuzzer`, not on PR replay. Neither path is a runtime sandbox. |
| YARA | `src/yaracpp`; `src/fileinfo/pattern_detector`; `src/bin2llvmir` provider init | Rule files plus the input binary | **No.** libyara runs in the decompiler process. |
| Unpacker emulation | `src/unpacker`, `src/unpackertool` (UPX/mpress stubs, NRV, unfilter) | Packed payload and decompressor | **No.** Emulation is in-process as the analyst user. |
| Neural prompt | `src/neural/prompts.cpp` | Decompiled text (may contain binary-lifted strings) | **No OS sandbox.** Prompts run `stripCStringLiterals` before the model sees function source (`tests/neural/mock_test.cpp`). |
| GUI subprocess | `src/gui` `QProcess` → `retdec-decompiler` | Same untrusted input as CLI | **No.** Out-of-process only. No seccomp, AppContainer, or `sandbox_init`. Child inherits host rights. |
| GPU kernel | `src/cuda_accel`, `src/opencl`; optional llama.cpp CUDA | Host buffers / model weights if those builds are enabled | **No.** `cuda_accel` / `opencl` are unintegrated and default **OFF**. llama.cpp GPU offload is a separate opt-in (`RETDEC_NEURAL_GPU_OFFLOAD`). Kernels are not isolated from the host. |

`SECURITY.md` tells operators to run untrusted binaries in an isolated VM
or container. That isolation is **operator-provided**, not built into
RetDec.

## GUI opening files

`src/gui/mainwindow.cpp` feeds attacker-controlled paths into the same
parsers as the CLI:

- **File → Open Binary…** (`onOpenBinary` / `QFileDialog::getOpenFileName`)
- **Drag-and-drop** (`setAcceptDrops(true)`; `dropEvent` calls `openBinary`
  or `openProject` for `.retdec`)
- **Command-line / argv** (`main.cpp` `window.openBinary(arg)`)
- **Re-open last binary** from settings
- **Binary Browser / Signature Studio / Diff** additional `QFileDialog`
  open paths

`openBinary` parses on disk in-process (`BinaryBrowserPanel::loadBinary`,
optional `fileinfo`) and may spawn `retdec-decompiler` as a `QProcess`
child with the analyst's rights. There is no content-type allow-list that
makes a dropped file safe. Plugins loaded by the GUI run as the GUI user
(`What is not a security boundary` below).

## Supply chain (release artefacts)

| Mechanism | Status |
|-----------|--------|
| Keyless Sigstore / cosign `.sigstore.json` on GitHub Release assets | **Yes.** `release-installers.yml` signs CycloneDX, Linux tarball, Windows NSIS `setup.exe` + portable zip, macOS tarball. `sign-release-sbom.yml` backfills missing bundles. `C-SIGSTORE`. |
| Authenticode on the Windows installer | **No.** Residual for government networks (Plan.md REL-05). |
| Apple Developer ID + notarisation | **No.** MAC-01 is ad-hoc `codesign`. Gatekeeper quarantines browser downloads; `install.sh` strips `com.apple.quarantine` (`C-MACOS-BUNDLE`). |
| Docker Hub `imortek/retdec` | **Unpublished** (`C-DOCKER-HUB`). GHCR pack path exists; unauthenticated pulls have returned 401. |
| Default Linux AppImage | **No.** Tarball is default; AppImage is `APPIMAGE=1` / leftover workflow (`C-APPIMAGE`). |

Operators who require Authenticode or notarised macOS builds must add that
out of tree. This document does not claim those signatures exist.

## Neural (N3, N5, N6)

- **Decompiled C is never executed** on the host.
- The compile-gate (`src/neural/gates.cpp`) spawns argv
  `{cc, "-fsyntax-only", "-w", src}` (Windows: `CreateProcess` of
  `gcc -fsyntax-only -w`). It does not link or run the translation unit.
  `RETDEC_NEURAL_GATE_CC` selects the compiler binary; there is no
  `std::system` / shell concatenation.
- If `RETDEC_NEURAL_DIFF_GATE` is set, the gate **warns and skips**.
  Runtime differential execution is treated as pass-without-run.
- `RETDEC_NEURAL_SKIP_COMPILE_GATE` skips the compile-gate.
- **N6 allowlist is fail-closed.** Shipped `support/models.json` pins
  Unsloth `Qwen3.5-9B-Q4_K_M.gguf`
  (`03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8`).
  SHA-256 is checked when `RETDEC_NEURAL_MODEL_SHA256` is set, or when
  the filename matches the pinned Qwen 3.5 Q4_K_M hint. Other GGUF paths
  are refused unless the hash is in the allowlist or
  `RETDEC_NEURAL_ALLOW_UNVERIFIED=1`
  (`tests/neural/mock_test.cpp` `UnpinnedOtherGgufRefusedWithoutAllowlist`,
  `TextGgufLoadsWhenUnverified`). Env SHA alone is not enough against an
  empty allowlist (`EmptyAllowlistRefusesEvenWithMatchingEnvSha`).
- Multimodal `mmproj` / `-VL-` names and CLIP/projector GGUF headers are
  rejected (`RejectsMultimodalMmprojFilename`,
  `StructuralRejectIgnoresUnverifiedAndFilename`).
- Prompt construction runs `stripCStringLiterals` on function source:
  C `"..."` / `'...'` bodies and comment text
  (`StripsStringLiteralsFromFunctionSource`,
  `StripsCommentBodiesFromFunctionSource`). Identifiers and
  `semanticContextJson` are not stripped.

Neural refinement itself is opt-in (`RETDEC_NEURAL_REFINE` + model path).
Release installers link llama.cpp; inference still does not run until
those env vars are set.

### Residual risks (neural)

- **Model poisoning.** Unpinned GGUF loads when
  `RETDEC_NEURAL_ALLOW_UNVERIFIED=1`. There is no signed model manifest.
  Env pin + an empty `models.json` still refuses
  (`EmptyAllowlistRefusesEvenWithMatchingEnvSha`). The shipped allowlist
  contains only the Unsloth Q4_K_M SHA.
- **Sidecar / cache poisoning.** `writeSidecar` writes
  `{output}.refined.c` and `{output}.refinement-manifest.json` with no
  MAC. Optional `RETDEC_NEURAL_CACHE_DIR` stores `{key}.txt` and reuses
  it verbatim (`CacheHitReusesAcceptedRefinement`); no HMAC.
- **Gate bypass.** `RETDEC_NEURAL_DIFF_GATE` skips (pass-without-run).
  Compile-gate is syntax-only (`-fsyntax-only -w`).
  `RETDEC_NEURAL_SKIP_COMPILE_GATE` skips it. Structural shape/spawn
  counts are not a sandbox.
- **Resource exhaustion.** `RETDEC_NEURAL_DEADLINE_MS` (default 0 = off)
  and `RETDEC_NEURAL_MAX_TOKENS` (else `GenerationConfig::maxTokens` =
  512) cap generation in-process. They are not a sandbox.
- **Prompt injection beyond literals.** After `stripCStringLiterals`,
  hostile identifiers, DWARF-derived names (`from_debug`, `real_name`,
  `source_file`, `demangled`), import/link names, and unstripped
  `semanticContextJson` still enter the prompt
  (`IncludesSemanticContextWhenSet`).
- **“Nothing leaves the machine”** is policy (`RETDEC_NO_NETWORK` /
  `RETDEC_NEURAL_OFFLINE_ONLY`; `RETDEC_NEURAL_ALLOW_NETWORK` overrides),
  not a verified syscall filter.

## What is not a security boundary

- Correctness of decompiled C, types, or semantic detections.
- Plugins (`IDecompilerPlugin` and friends) — loaded code runs as the GUI
  user.
- Optional `RETDEC_NO_NETWORK=1` / `RETDEC_NEURAL_OFFLINE_ONLY` — policy
  flags, not a verified syscall filter (audit S17 is open).

## Residual risk (honest)

Parsers, YARA, and unpacker emulation share the analyst process. A
malicious sample can attempt memory corruption, huge allocations, or
hostile rule/data interaction. Opening a file in the GUI is the same
trust boundary as passing it to the CLI. The GUI child can write wherever
the user can. Until S15 (sandboxed worker) exists, treat RetDec as an
unsandboxed native analysis tool. N6 is fail-closed: unknown GGUF hashes
are refused; the shipped allowlist contains only the pinned Unsloth
Qwen 3.5 Q4_K_M SHA. Release assets have Sigstore attestations and do
**not** have Authenticode. The GGUF itself is split Release assets, not
inside the OS installer archives.
