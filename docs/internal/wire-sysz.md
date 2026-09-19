# SystemZ (`createSysz`) — 64-bit s390x Production

Do **not** invent a 31-bit Capstone mode. Capstone 6 `CS_ARCH_SYSZ` /
`SYSTEMZ` has **no** 31-bit / ESA-390 `CS_MODE_*`. Production is **64-bit
s390x** (z/Architecture GPRs). Instruction bytes are always big-endian.

`createSysz(m, extra)` opens with basic `CS_MODE_BIG_ENDIAN`. Extra must
be `CS_MODE_LITTLE_ENDIAN` (0, the default) or a Capstone 6
`CS_MODE_SYSTEMZ_ARCH*` / `CS_MODE_SYSTEMZ_Z*` generation bit. Passing
`CS_MODE_BIG_ENDIAN` again is coerced to 0 — `1U<<31 + 1U<<31` wraps to
little-endian and 4-byte insns such as `LGR` fail to decode. With extra
0, Capstone’s default is “all features” (vector included).

Capstone 6 names (real tokens in `systemz.h` / `systemz_compatibility.h`
when `CAPSTONE_SYSTEMZ_COMPAT_HEADER` is on):

| Capstone 5 token | Capstone 6 token |
| --- | --- |
| `SYSZ_REG_0` … `SYSZ_REG_15` | `SYSZ_REG_R0D` … `SYSZ_REG_R15D` |
| `SYSZ_REG_F0` … | `SYSZ_REG_F0D` (long BFP) / `SYSZ_REG_F0S` (short BFP) |
| `SYSZ_INS_JG*` | `SYSZ_INS_J` / `SYSZ_INS_J_G_LU_` / `SYSZ_INS_J_G_L_*` |
| `SYSZ_OP_ACREG` | gone; access regs are `SYSZ_OP_REG` (`SYSZ_REG_A*`) |
| `sysz_cc` | `systemz_cc` (`SYSTEMZ_CC_*`); `cs_sysz` is `cs_systemz` |

`src/capstone2llvmir/capstone6_compat.h` aliases the 5.x GPR/FPR names onto
`R*D` / `F*D`. The translator maps **both** `F*S` (float) and `F*D` (double).
32-bit `R*L` / `R*H` operand IDs overlay the same `R*D` globals.

Sources:

- `include/retdec/capstone2llvmir/sysz/sysz.h`
- `src/capstone2llvmir/sysz/sysz_impl.h`
- `src/capstone2llvmir/sysz/sysz.cpp`
- `src/capstone2llvmir/sysz/sysz_init.cpp`
- `tests/capstone2llvmir/sysz_tests.cpp`
- `include/retdec/bin2llvmir/providers/abi/sysz.h`
- `src/bin2llvmir/providers/abi/sysz.cpp`

`createArch(CS_ARCH_SYSZ, …)` calls `createSysz(m, extra)`.

## ABI (ELF s390x)

`AbiSysz` models r2–r6 arguments, r2 return, r14 return address, r15 SP.

## Mapped Capstone IDs

**Integer / control-flow:** `LR`, `LGR`, `LTR`, `LTGR`, `AR`, `AGR`, `SR`,
`SGR`, `NR`, `OR`, `XR`, `NGR`, `OGR`, `XGR`, `L`, `LG`, `ST`, `STG`, `LA`,
`LAY`, `LARL`, `LGHI`, `LHI`, `AGHI`, `AHI`, `CR`, `CGR`, `CHI`, `CGHI`,
`LGFR`, `LLGFR`, `LGF`, `LLGF`, `SLLG`, `SRLG`, `SRAG`, `BR`, `BCR`, `BRC`,
`BRCL`, `J`, `J_G_LU_`, `J*` / `J_G_L_*` condition aliases, `BASR`, `BRAS`,
`BRASL`.

**BFP (IEEE) scalar:** `AEBR`/`AEB`, `ADBR`/`ADB`, `SEBR`/`SEB`,
`SDBR`/`SDB`, `MEEBR`/`MEEB`, `MDBR`/`MDB`, `DEBR`/`DEB`, `DDBR`/`DDB`,
`CEBR`/`CEB`, `CDBR`/`CDB`, `LDEB`/`LDEBR`, `LEDBR`, `LE`/`LEY`, `LD`/`LDY`,
`STE`/`STEY`, `STD`/`STDY`, `LER`, `LDR`.

**Vector load/store (gcc -O1):** `VL`, `VST`, `VLR`, `VLREP`/`VLREPB`/`H`/`F`/`G`,
`VLEG`/`VLEF`, `VSTEG`/`VSTEF`.

GPR 0 as a base/index contributes 0 (z/Architecture). `BR`/`BASR` to r0
are nops.

## Remaining gaps (pseudo-asm)

- 31-bit / ESA-390 (no Capstone mode)
- Hexadecimal FP (`AE`, `AD`, `ME`, `DE` without `B`) and DFP (`ADTR`, …)
- BFP convert/round/test (`CGEBR`, `CFEBR`, `FIDBR`, `TCEB`, `SQEBR`,
  fused `MAEBR`, …)
- Vector arithmetic (`VA`, `VS`, `VN`, `VFA`, packed-decimal, …)
- String / MVC / CLC / XC, `LM`/`STM`/`LMG`/`STMG`
- CS/CSG and interlocked-access (`LAA`, …)
- Privileged, transactional-execution, MSA crypto

## Tests

`tests/capstone2llvmir/sysz_tests.cpp` uses **real bytes** (`emulate_bin`)
for the integer core plus AEBR/ADBR/MEEBR/LDEB/CEBR, LD/STD, LGHI/AGHI/NGR/CGR/LAY,
and VL/VLR. `createSysz(&_module)` (extra 0).
