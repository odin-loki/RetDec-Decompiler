# Integrator patches — NVIDIA SASS / cubin

Status of the library that already exists in-tree, and the exact diffs
still required to reach `retdec-decompiler` input. **This is not
Production.** SASS is not in Capstone. `nvdisasm` is not called or
bundled (CUDA EULA).

Implemented today:

| Piece | Path | Wired into `decompile()`? |
|-------|------|---------------------------|
| cubin ELF loader (`e_machine == 190`) | `src/sass_decode/cubin_loader.cpp` | **No** |
| fatbin header + payload scan | same | **No** |
| SM_70/SM_80 subset decoder | `src/sass_decode/sass_decoder.cpp` | **No** |
| CUDA-C subset + PTX fallback | `src/sass_decode/sass_lifter.cpp` | **No** |
| unit tests | `tests/sass_decode/` | n/a (ctest) |
| CMake library + tests | `src/CMakeLists.txt`, `tests/CMakeLists.txt` | yes (build) |

32 vs 64 is **GPU pointer width** (`SassConfig::pointerBits` / ELF class),
not a CPU `-a` token. Do **not** add `-a sass` / `-a sass32` unless a later
task explicitly wants Capstone-shaped CLI; SASS will never go through
Capstone.

---

## Citations (do not invent)

- `EM_CUDA = 190` — in-tree `deps/elfio/include/elfio/elf_types.hpp`; ELF gABI.
- cubin is ELF — NVIDIA CUDA Binary Utilities
  https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html
- fatbin magic / header — NVIDIA CUDA Toolkit `fatbinary.h`
  (`FATBIN_MAGIC 0xBA55ED50`, `fatBinaryHeader`, `FATBIN_KIND_PTX/ELF`,
  `FATBIN_FLAG_64BIT`). Per-member records after that header are NVIDIA
  **internal**; the loader scans for ELF/PTX instead of inventing them.
- Opcode bytes — NVIDIA-printed encodings in CUDA Binary Utilities 12.8.2
  PDF and 13.3 HTML (EXIT `…794d`, NOP `…7918`, IADD3 `…7210`, …).
- Pre-Volta 8-byte EXIT — CUDA Binary Utilities 9.1 (`0x8000000000001de7`).

`fileinfo` already pretty-prints `EM_CUDA` in
`src/fileinfo/file_detector/elf_detector.cpp`. `ElfFormat::getTargetArchitecture()`
does **not** map it (falls through to `Architecture::UNKNOWN`). That is
correct until `Architecture::CUDA` exists.

---

## 1. CMakeLists — already applied

`src/CMakeLists.txt` (after `ptx_decompile`):

```cmake
add_subdirectory(sass_decode)
```

`tests/CMakeLists.txt`:

```cmake
add_subdirectory(sass_decode)
```

Library: `retdec-sass-decode` (links `retdec-ptx-decompile` for fatbin PTX).
Test binary: `retdec-sass-decode-tests`.

---

## 2. Decoder — already applied

Public headers:

- `include/retdec/sass_decode/sass_types.h`
- `include/retdec/sass_decode/cubin_loader.h`
- `include/retdec/sass_decode/sass_decoder.h`
- `include/retdec/sass_decode/sass_lifter.h`

API:

```cpp
SassModule loadCudaBinary(data, size, SassConfig{});
SassDecoder dec(mod.sm, mod.pointerBits);
dec.decodeModule(mod);
std::string cudaC = SassLifter{}.liftModule(mod);
// or
std::string cudaC = decompileCudaBinary(data, size, cfg);
```

`SassConfig::pointerBits`: `0` = ELFCLASS32 → 32-bit GPU pointers, ELF64 →
64-bit; `32` or `64` overrides. This is SM addressing.

---

## 3. fileformat — **not** applied (copy these)

Do **not** add `Architecture::CUDA` to `include/retdec/fileformat/fftypes.h`
in the same change as the probe. Every `switch (Architecture)` would need a
new arm; missing one miscompiles or lies. Keep cubin out of Capstone.

### 3a. `format_lattice.cpp` `elfMachineToArch`

File: `src/fileformat/lattice/format_lattice.cpp`

```cpp
static Arch elfMachineToArch(uint16_t machine)
{
	switch (machine)
	{
	case 0x03: return Arch::X86;
	case 0x3E: return Arch::X86_64;
	case 0x28: return Arch::ARM;
	case 0xB7: return Arch::AArch64;
	case 0x08: return Arch::MIPS;
	case 0x14: return Arch::PowerPC;
	case 0x15: return Arch::PowerPC64;
	case 0xF3: return Arch::RISC_V;
	case 190:  return Arch::Unknown; // EM_CUDA — not a lattice Arch; see sass_decode
	default: return Arch::Unknown;
	}
}
```

Optional: stash `e_machine` on `FormatResult` (new field) so callers can see
190. That is a public-struct change; do it in its own commit.

### 3b. `ElfFormat::getTargetArchitecture`

File: `src/fileformat/file_format/elf/elf_format.cpp`

Leave `EM_CUDA` on the `default → UNKNOWN` path **or** add an explicit case
with a comment. Do not return `X86_64` for cubin.

```cpp
		case EM_CUDA:
			// NVIDIA cubin. Not a fileformat::Architecture enumerator.
			return Architecture::UNKNOWN;
```

### 3c. `ElfFormat::getBytesPerWord`

Same file, same switch. Optional, GPU pointer width only:

```cpp
		case EM_CUDA:
			return (elfClass == ELFCLASS64) ? 8 : 4;
```

### 3d. Fatbin in host PE/ELF

Host binaries embed fatbins in `.nv_fatbin` / `.nvFatBinSegment`. A later
pass can scan those sections and call `loadFatbin`. Not required for
standalone `.cubin` / `.fatbin` files. Do not treat host x86 as SASS.

---

## 4. CLI — **not** applied (copy these)

**Do not add `-a sass`.** Capstone `createArch` has no SASS. A fake arch
token would send cubin into `bin2llvmir` and produce garbage.

Wire as an **early input-keyed route**, same idea as `.wasm` / `.luac`
in `managed_decompiler.cpp`, but cubin is not bytecode-VM managed code.
Preferred: probe in `src/retdec-decompiler/retdec-decompiler.cpp` after the
file is read and **before** the native LLVM pipeline.

### 4a. `src/retdec-decompiler/CMakeLists.txt`

Add the library next to the other non-Capstone parsers:

```cmake
	retdec-wasm-parser
	retdec-sass-decode
```

### 4b. `retdec-decompiler.cpp` (after the managed-format probe, before native)

Include:

```cpp
#include "retdec/sass_decode/cubin_loader.h"
#include "retdec/sass_decode/sass_lifter.h"
```

Probe (use the same bytes already slurped for `detectManagedFormatFromBytes`).
If that buffer is only a prefix, slurp the whole file for SASS:

```cpp
		if (retdec::sass_decode::looksLikeFatbin(managedProbeBytes.data(),
		                                         managedProbeBytes.size())
		    || retdec::sass_decode::looksLikeCubin(managedProbeBytes.data(),
		                                          managedProbeBytes.size()))
		{
			std::ifstream ifs(config.parameters.getInputFile(), std::ios::binary);
			std::vector<uint8_t> all((std::istreambuf_iterator<char>(ifs)),
			                         std::istreambuf_iterator<char>());
			retdec::sass_decode::SassConfig scfg;
			// Optional: scfg.pointerBits from --bit-size if the user set 32 or 64.
			std::string cudaC = retdec::sass_decode::decompileCudaBinary(
				all.data(), all.size(), scfg);
			std::ofstream ofs(config.parameters.getOutputFile());
			ofs << cudaC;
			return 0;
		}
```

`--bit-size` already exists (`-b|--bit-size` 16|32|64). If you honour it for
cubin, map 32/64 → `SassConfig::pointerBits` and **reject 16**. That is
pointer width, not a fake `-a`.

### 4c. Help text

Add a sentence near the managed-input list, not to the `-a` allow-list:

```
Standalone NVIDIA cubin (ELF e_machine 190) and fatbin (magic 0xBA55ED50)
are decoded by src/sass_decode (subset, not Production). Not a -a architecture.
```

Do **not** extend

```
[-a|--arch ARCH] ... [mips|pic32|arm|thumb|arm64|powerpc|x86|x86-64]
```

with `sass`.

### 4d. Optional `ManagedFormat` alternative

If you would rather sit next to wasm:

`managed_decompiler.h`:

```cpp
    Wasm,
    CliAssembly,
    CudaCubin,   ///< ELF EM_CUDA cubin
    CudaFatbin,  ///< NVIDIA fatbin 0xBA55ED50
```

`detectManagedFormatFromBytes`: after the wasm/lua probes, **before** PE:

```cpp
	if (retdec::sass_decode::looksLikeFatbin(d, size)) return ManagedFormat::CudaFatbin;
	if (retdec::sass_decode::looksLikeCubin(d, size)) return ManagedFormat::CudaCubin;
```

`decompileManaged`: new `decompileCudaGpu` that slurps + `decompileCudaBinary`.

`output_lang.cpp`: treat as CUDA-C / `.cu` (there is no Production `--output-lang cuda` today).

This still must not go through Capstone.

---

## 5. Tests — already applied

`tests/sass_decode/sass_decode_test.cpp` uses **byte arrays** (NVIDIA listing
words) and synthetic ELF/fatbin. No CUDA toolkit download, no checked-in cubin
blobs (none existed in-tree).

```
ctest -R retdec-sass-decode-tests --output-on-failure
```

After CLI wiring, add a decompiler smoke that writes a synthetic cubin to a
temp file and checks the `.c`/`.cu` contains `__global__` and `return;`.
Do not mark that smoke as Production.

---

## 6. Gaps — Production is **not** reachable

1. **No public NVIDIA bit-accurate ISA.** CUDA Binary Utilities lists
   mnemonics and prints example encodings. Full operand forms, control codes,
   uniform datapath, tensor ops, and SM-specific variants are not specified
   as an encoding document. The decoder claims only the matched subset.
2. **`nvdisasm` is EULA-encumbered.** Using it as a preprocessor is
   disallowed here and would not produce LLVM IR anyway.
3. **Capstone has no SASS.** `bin2llvmir` / llvmir2hll cannot be the e2e path.
4. **Compressed fatbin** (`FATBIN_FLAG_COMPRESS`) is not inflated.
   NVIDIA compression is not a public ISA.
5. **Fatbin member directory** after `fatBinaryHeader` is NVIDIA-internal.
6. **Host-embedded** `.nv_fatbin` in x86/ARM PE/ELF is not scanned.
7. **CLI / fileformat / `decompile()`** are unwired (this file).
8. **Quality bar:** even a wired subset lift is comments + a handful of
   C operators, not recovered CUDA kernels at x86 Production quality.

Until (7) is applied, e2e decode→C is a **library call**, not
`retdec-decompiler`. Until (1) is solved without nvdisasm, Production is
the wrong word even after (7).

---

## 7. README / status docs

- README architecture table: **do not** add SASS as Production.
- `docs/CUDA_CAPABILITIES.md` / `docs/ARCHITECTURE_TARGETS.md` describe the
  loader + subset and say **not** in `decompile()`.
