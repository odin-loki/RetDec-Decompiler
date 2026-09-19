# Integrator patches — NVIDIA SASS / cubin

Status of the library that already exists in-tree, and the exact diffs
still required to reach `retdec-decompiler` input. **This is not
Production.** SASS is not in Capstone. `nvdisasm` is not called or
bundled (CUDA EULA).

Implemented today:

| Piece | Path | Wired into `decompile()`? |
|-------|------|---------------------------|
| cubin ELF loader (`e_machine == 190`) | `src/sass_decode/cubin_loader.cpp` | CLI probe only (not Capstone) |
| fatbin header + payload scan | same | CLI probe only |
| SM_70/SM_80 subset decoder | `src/sass_decode/sass_decoder.cpp` | CLI probe only |
| CUDA-C subset + PTX fallback | `src/sass_decode/sass_lifter.cpp` | CLI probe only |
| unit tests | `tests/sass_decode/` | n/a (ctest) |
| CMake library + tests | `src/CMakeLists.txt`, `tests/CMakeLists.txt` | yes (build) |

Decoded opcode bytes are **only** those NVIDIA printed (low 8 bits of word0).
Newly lifted: **IMAD.WIDE** (`0x25`), **SHFL** (`0x89`), **S2UR** (`0xc3`), **LDCU** (`0xac`).
Already lifted: EXIT, NOP, BRA, IMAD, MOV, LDG, STG, IADD3, FADD, S2R, LDC.

**Still missing** for nvcc `-O1` kernels (ISA-table mnemonics, **no** printed
encoding word — not invented): **FMUL, FFMA, ISETP, SHL, SHR, LOP3**, plus
FSETP, MUFU, LEA, BRA displacement, S2R/S2UR SR selector. **Not Production.**
A 2026-09-19 re-search of CUDA Binary Utilities 8.0–13.3 found **no** new
16-byte words for those mnemonics (see § Citations / encoding search).

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
  PDF and 13.3 HTML (EXIT `…794d`, NOP `…7918`, IADD3 `…7210`, IMAD `…7624` /
  `…7c24`, IMAD.WIDE `…7825`, MOV `…7a02`, LDG `…7381`/`…7981`, STG `…7386`/
  `…7986`, FADD `…7221`, S2R `…7919`, LDC `…7b82`, SHFL `…f389`, S2UR `…79c3`,
  LDCU `…77ac`, BRA `…7947`). FMUL/FFMA/ISETP/SHL/SHR/LOP3/FSETP/MUFU/LEA
  have **no** printed word in those listings (reconfirmed 2026-09-19).
- Pre-Volta 8-byte EXIT — CUDA Binary Utilities 9.1 (`0x8000000000001de7`).
  The same Fermi `-hex` sample also prints MOV (`…5de4` / `…9de4` / …),
  LDU.E (`…1c85` with high `0x8c…`), IADD (`…1c03`), ST.E (`…1c85` with
  high `0x94…`). Those are **8-byte Fermi** encodings, not Volta+ 16-byte
  words; LDU and ST share the low 16 bits, so they are **not** claimed on
  the SM_70 decoder. Not FMUL/FFMA/ISETP/SHL/SHR/LOP3/FSETP/MUFU/LEA.

### Encoding search (2026-09-19) — no new 16-byte words

Consulted NVIDIA CUDA Binary Utilities (HTML and, where fetched, PDF).
`nvdisasm` was **not** run. Third-party assemblers were **not** used.

| Document | URL | Printed `/* 0x… */` next to a mnemonic? | FMUL/FFMA/ISETP/SHL/SHR/LOP3/FSETP/MUFU/LEA encoding word? |
|----------|-----|------------------------------------------|-----------------------------------------------------------|
| 13.3 current | https://docs.nvidia.com/cuda/cuda-binary-utilities/index.html | Yes — LDC/S2R/S2UR/LDCU/IMAD/IMAD.WIDE/LDG/FADD/STG/EXIT/BRA/NOP | **No.** ISA tables name them. A second sample lists FMUL.FTZ / FFMA.FTZ / FSETP / MUFU.SQRT / ISETP **without** hex words (not `-hex`). |
| 13.1.2 | https://docs.nvidia.com/cuda/archive/13.1.2/cuda-binary-utilities/index.html | Same 13.x `-hex` kernel as 13.3 | **No** |
| 13.0.0 | https://docs.nvidia.com/cuda/archive/13.0.0/cuda-binary-utilities/index.html | Same 13.x `-hex` kernel | **No** |
| 12.8.0 HTML | https://docs.nvidia.com/cuda/archive/12.8.0/cuda-binary-utilities/index.html | Yes — IMAD.MOV / SHFL / MOV / LDG / IADD3 / STG / EXIT / BRA / NOP | **No.** Same non-hex FMUL/FFMA/FSETP/MUFU/ISETP control-flow sample. |
| 12.8.2 PDF | https://docs.nvidia.com/cuda/archive/12.8.2/pdf/CUDA_Binary_Utilities.pdf | Same 12.8 `-hex` kernel | **No** |
| 12.6.0 | https://docs.nvidia.com/cuda/archive/12.6.0/cuda-binary-utilities/index.html | Same 12.x `-hex` kernel | **No** |
| 12.4.0 / 12.4.1 PDF | https://docs.nvidia.com/cuda/archive/12.4.0/cuda-binary-utilities/index.html , https://docs.nvidia.com/cuda/archive/12.4.1/pdf/CUDA_Binary_Utilities.pdf | Same 12.x `-hex` kernel | **No.** FMUL/FFMA appear as mnemonics only. |
| 12.1.0 | https://docs.nvidia.com/cuda/archive/12.1.0/cuda-binary-utilities/index.html | Same 12.x `-hex` kernel | **No** |
| 10.2 | https://docs.nvidia.com/cuda/archive/10.2/cuda-binary-utilities/index.html | Fermi 8-byte MOV/LDU/IADD/ST/EXIT only | **No** 16-byte FMUL/… |
| 9.1 | https://docs.nvidia.com/cuda/archive/9.1/cuda-binary-utilities/index.html | Same Fermi 8-byte sample (`EXIT` `0x8000000000001de7`) | **No** |
| 8.0 | https://docs.nvidia.com/cuda/archive/8.0/cuda-binary-utilities/index.html | Same Fermi 8-byte sample | **No** |

Volta+ 16-byte words NVIDIA **did** print (already decoded; low 8 bits):

| Mnemonic | word0 (NVIDIA print) | op | Document |
|----------|----------------------|----|----------|
| EXIT | `0x000000000000794d` | `0x4d` | 12.8.2 / 13.3 |
| NOP | `0x0000000000007918` | `0x18` | 12.8.2 / 13.3 |
| BRA | `0xfffffff000007947` / `0xfffffffc00fc7947` | `0x47` | 12.8.2 / 13.3 |
| IMAD / IMAD.MOV.U32 | `0x00000a00ff017624` / `0x0000000600097c24` | `0x24` | 12.8.2 / 13.3 |
| IMAD.WIDE | `0x0000000409027825` | `0x25` | 13.3 |
| MOV | `0x0000590000037a02` | `0x02` | 12.8.2 |
| LDG | `0x0000000002027381` / `0x0000000402027981` | `0x81` | 12.8.2 / 13.3 |
| STG | `0x0000000906007386` / `0x0000000906007986` | `0x86` | 12.8.2 / 13.3 |
| IADD3 | `0x0000000502097210` | `0x10` | 12.8.2 |
| FADD | `0x0000000502097221` | `0x21` | 13.3 |
| S2R | `0x0000000000097919` | `0x19` | 13.3 |
| LDC / LDC.64 | `0x0000df00ff017b82` / `0x0000e000ff027b82` | `0x82` | 13.3 |
| SHFL.IDX | `0x000000fffffff389` | `0x89` | 12.8.2 |
| S2UR | `0x00000000000679c3` | `0xc3` | 13.3 |
| LDCU.64 | `0x00006b00ff0477ac` | `0xac` | 13.3 |

**Missing (not invented):** FMUL, FFMA, ISETP, SHL, SHR, LOP3, FSETP, MUFU, LEA.
Until NVIDIA prints a 16-byte example for those, SASS stays **not Production**.

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
7. **fileformat `Architecture::CUDA`** is still absent (correct). CLI cubin/fatbin
   probe exists in `retdec-decompiler.cpp`; it is not a `-a` token.
8. **Compiler ops without printed encodings:** FMUL, FFMA, ISETP, SHL, SHR, LOP3
   stay `Unknown` (hex comments). Do not invent bytes.
9. **Quality bar:** the subset lift is comments + a handful of C operators, not
   recovered CUDA kernels at x86 Production quality.

Until (1) is solved without nvdisasm, Production is the wrong word. nvcc `-O1`
integer+FP kernels need FMUL/FFMA/ISETP/LOP3/SHF encodings NVIDIA has not printed.

---

## 7. README / status docs

- README architecture table: **do not** add SASS as Production.
- `docs/CUDA_CAPABILITIES.md` / `docs/ARCHITECTURE_TARGETS.md` describe the
  loader + subset and say **not** in `decompile()`.
