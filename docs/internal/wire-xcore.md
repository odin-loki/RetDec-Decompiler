# Wire XCore into `createXcore`

**There is no 64-bit XCore.** The XS1/XS2 ISA is 32-bit. Capstone 5.0.9
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
