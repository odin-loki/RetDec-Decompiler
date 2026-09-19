# Wire XCore into `createXcore`

**There is no 64-bit XCore.** The XS1/XS2 ISA is 32-bit. Capstone 6.0.0-Alpha10
`CS_ARCH_XCORE` has no `CS_MODE_64` / `CS_MODE_XCORE_64`. Do not invent one.
`getArchByteSize()` is 4. Capstone’s own tests open the engine with
`CS_MODE_BIG_ENDIAN` for 16-bit instruction-word byte order; data memory is
little-endian (`e-p:32:32:32`).

Sources already in the tree:

- `include/retdec/capstone2llvmir/xcore/xcore.h`
- `src/capstone2llvmir/xcore/xcore_impl.h`
- `src/capstone2llvmir/xcore/xcore.cpp`
- `src/capstone2llvmir/xcore/xcore_init.cpp`
- `tests/capstone2llvmir/xcore_tests.cpp`
- `include/retdec/bin2llvmir/providers/abi/xcore.h`
- `src/bin2llvmir/providers/abi/xcore.cpp`

CMake already lists the translator, tests, and ABI sources.

## Production integer / control-flow map

`_i2fm` lists every Capstone 6.0.0-Alpha10 `XCORE_INS_*` (from the installed
`capstone/xcore.h`). Mapped IDs emit real LLVM IR.

Channel I/O is modeled like x86 `translateIns`/`translateOuts`: named module
helpers created with `getPseudoAsmFunction`, returning SSA values that are
stored to the dest register. Names are `xcore.chan.*` / `xcore.res.*` /
`xcore.event.*` / `xcore.thread.*`, not opaque `__asm_in` barriers. The
channel/resource operand is a register (Capstone `MEM` `res[rN]`, or parsed
from `op_str` when the printer overwrites that MEM).

### Mapped (LLVM IR)

| Group | IDs |
|---|---|
| Integer ALU | `ADD`, `SUB`, `AND`, `ANDNOT`, `OR`, `XOR`, `NOT`, `NEG`, `MUL`, `SHL`, `SHR`, `ASHR`, `EQ`, `LSS`, `LSU`, `CLZ`, `BITREV`, `BYTEREV`, `MKMSK`, `SEXT`, `ZEXT` |
| Divide / rem | `DIVS`, `DIVU`, `REMS`, `REMU` (zero and `INT_MIN/-1` are defined, not poison) |
| Long / MAC / CRC | `LADD`, `LSUB`, `LMUL`, `LDIVU`, `MACCS`, `MACCU`, `CRC32`, `CRC8` |
| Address / stack | `LDC`, `LDAW`, `LDA16`, `LDAP`, `LDW`, `LD16S`, `LD8U`, `STW`, `ST16`, `ST8`, `ENTSP`, `EXTSP`, `EXTDP`, `RETSP`, `KENTSP`, `KRESTSP`, `DENTSP`, `DRESTSP` |
| Control flow | `BU`, `BAU`, `BRU`, `BF`, `BT`, `BL`, `BLA`, `BLAT`, `DCALL`, `DRET`, `KCALL`, `KRET`, `ECALLF`, `ECALLT` |
| Thread status / GPRs | `GET` (reg←reg), `SET` (reg←reg), `GETSR`, `SETSR`, `CLRSR`, `SSYNC` (nop) |
| Channel I/O | `IN`, `OUT`, `INPW`, `OUTPW`, `INSHR`, `OUTSHR`, `INCT`, `OUTCT`, `INT`, `OUTT`, `PEEK`, `ENDIN`, `TESTCT`, `TESTWCT` |
| Resource | `SETD`, `SETC`, `SETCLK`, `SETPT`, `SETTW`, `SETV`, `SETEV`, `GETD`, `GETTS`, `GETR`, `GETN`, `FREER` |
| Events | `EEU`, `EDU`, `EEF`, `EET`, `WAITEU`, `WAITEF`, `WAITET`, `CLRE` |
| Threads / sync | `START`, `MSYNC`, `MJOIN`, `SYNCR` |

### Remaining gaps (`nullptr`)

True hardware-only ops with no sequential model (generic `__asm_*` pseudo):

`CHKCT` (control-token check that raises a hardware exception), `FREET`
(kill current thread), `CLRPT`, `DGETREG`, `INIT`, `GETST`, `SETN`,
`SETPSC`, `SETRDY`, `TESTLCL`, `TSETMR`.

`GET`/`SET` of `ps[reg]` (processor state memory) also fall back to
pseudo-asm; only register-to-register forms are lifted.

Capstone’s XCore printer often leaves a trailing `XCORE_OP_MEM` in
`operands[op_count]` without incrementing `op_count`. Load/store/LDAW/LDA16
treat that extra MEM as a real operand. `SETSP`/`SETDP`/`SETCP` print the
destination as a literal; the translator uses `regs_write` (and the printed
`sp`/`dp`/`cp` token) for the dest.

## Exact replacement: `src/capstone2llvmir/capstone2llvmir.cpp`

### 1. Include (after the SystemZ impl include once that hunk is in)

```diff
 #include "capstone2llvmir/powerpc/powerpc_impl.h"
 #include "capstone2llvmir/sysz/sysz_impl.h"
+#include "capstone2llvmir/xcore/xcore_impl.h"
 #include "capstone2llvmir/x86/x86_impl.h"
```

If SystemZ is not wired in the same edit, add the XCore include next to the
other `*_impl.h` lines:

```diff
 #include "capstone2llvmir/powerpc/powerpc_impl.h"
+#include "capstone2llvmir/xcore/xcore_impl.h"
 #include "capstone2llvmir/x86/x86_impl.h"
```

### 2. Replace `createXcore` (currently throws `GenericError`)

**Remove:**

```cpp
std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createXcore(
		llvm::Module* m,
		cs_mode extra)
{
	throw GenericError("XCore architecture is unimplemented.");
	return nullptr;
}
```

**Insert:**

```cpp
std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createXcore(
		llvm::Module* m,
		cs_mode extra)
{
	return std::make_unique<Capstone2LlvmIrTranslatorXcore_impl>(
			m,
			CS_MODE_LITTLE_ENDIAN,
			extra);
}
```

`createArch(CS_ARCH_XCORE, …)` already calls `createXcore(m, extra)`.
Unit tests pass `CS_MODE_BIG_ENDIAN` as `extra` so `cs_open` matches
Capstone’s XCore test corpus.

## ABI (XS1)

`AbiXcore`: r0–r3 args, r0 return, `lr` return address, `sp` stack.
32-bit only. `AbiProvider` does not construct it until a later
architecture-detect wire.

## Tests

`tests/capstone2llvmir/xcore_tests.cpp` uses **real bytes** from the LLVM
XCore disassembler corpus (`emulate_bin`). Keystone has no XCore backend.
They call `createXcore` and will fail until this hunk is applied.
