# Wire SystemZ into `createSysz`

Do **not** invent a 31-bit Capstone mode. Capstone 5.0.9 `CS_ARCH_SYSZ` has
**no** 31-bit / ESA-390 `CS_MODE_*`. Production is **64-bit s390x**
(z/Architecture GPRs). Extra mode is endianness only; instruction bytes are
always big-endian. `createSysz(m, extra)` opens with
`CS_MODE_BIG_ENDIAN + extra`.

Sources already in the tree:

- `include/retdec/capstone2llvmir/sysz/sysz.h`
- `src/capstone2llvmir/sysz/sysz_impl.h`
- `src/capstone2llvmir/sysz/sysz.cpp`
- `src/capstone2llvmir/sysz/sysz_init.cpp`
- `tests/capstone2llvmir/sysz_tests.cpp`
- `include/retdec/bin2llvmir/providers/abi/sysz.h`
- `src/bin2llvmir/providers/abi/sysz.cpp`

CMake already lists the translator, tests, and ABI sources.

## Exact replacement: `src/capstone2llvmir/capstone2llvmir.cpp`

### 1. Include (after the PowerPC impl include)

```diff
 #include "capstone2llvmir/mips/mips_impl.h"
 #include "capstone2llvmir/powerpc/powerpc_impl.h"
+#include "capstone2llvmir/sysz/sysz_impl.h"
 #include "capstone2llvmir/x86/x86_impl.h"
```

### 2. Replace `createSysz` (currently throws `GenericError`)

**Remove:**

```cpp
std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createSysz(
		llvm::Module* m,
		cs_mode extra)
{
	throw GenericError("SystemZ architecture is unimplemented.");
	return nullptr;
}
```

**Insert:**

```cpp
std::unique_ptr<Capstone2LlvmIrTranslator> Capstone2LlvmIrTranslator::createSysz(
		llvm::Module* m,
		cs_mode extra)
{
	return std::make_unique<Capstone2LlvmIrTranslatorSysz_impl>(
			m,
			CS_MODE_BIG_ENDIAN,
			extra);
}
```

`createArch(CS_ARCH_SYSZ, …)` already calls `createSysz(m, extra)`.

## ABI (ELF s390x)

`AbiSysz` models r2–r6 arguments, r2 return, r14 return address, r15 SP.
`AbiProvider` does not construct it until a later architecture-detect wire;
the class compiles on its own.

## Tests

`tests/capstone2llvmir/sysz_tests.cpp` uses **real bytes** (`emulate_bin`) for
LR/LGR, AR/AGR, SR/SGR, NR/OR/XR, L/ST/LG/STG, BR/BRC/BRCL, BASR/BRASL, LA.
They call `createSysz` and will fail until this hunk is applied.
