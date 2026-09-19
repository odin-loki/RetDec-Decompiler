# PowerPC 32-bit and 64-bit — Production wiring

Status of the Capstone 5.0.9 → LLVM IR translator (`createPpc32` /
`createPpc64`) and the `bin2llvmir` ABI tables. Decoder, CLI, and
`architecture.h` are out of scope here.

## Production bar (this task)

Real LLVM IR (no `__asm_*` pseudo-call) for:

| Family | 32-bit (`CS_MODE_32`) | 64-bit (`CS_MODE_64`) |
|--------|----------------------|------------------------|
| `add` / `addi` / `addis` / `subf` / `and` / `or` / `xor` / `nand` | yes | yes (full GPR width) |
| `cmp` / `cmpi` / `cmpl` (+ `cmpw`/`cmpwi`/`cmplw`/`cmplwi`, `cmpd`/`cmpdi`/`cmpld`/`cmpldi`) | yes | yes; word forms narrow to i32; primary 4-op form honours `L` |
| `lwz`/`stw`/`lbz`/`stb`/`lhz`/`sth` | yes | yes (zero-extended into the 64-bit GPR) |
| `ld`/`std` | encodings do not exist | yes |
| `b` / `bl` / `blr` / `bctr` / `bc` | yes | yes |
| `mflr`/`mtlr`/`mfctr`/`mtctr` | yes | yes (LR/CTR are i64) |
| `rlwinm` / `rlwnm` / `rlwimi` | yes (32-bit rotate + mask) | yes (low word, result zero-extended) |
| `rldicl` / `rldicr` / `rldic` / `rldimi` / `rldcl` / `rldcr` | n/a | yes |
| compiler aliases `clrldi` / `rotldi` / `rotld` / `sldi` | n/a | yes |

Altivec (`v*`) and VSX (`xx*`/`xs*`/`xv*`) stay pseudo-asm, as allowed.

## Tests

`tests/capstone2llvmir/powerpc_tests.cpp` is instantiated with
`CS_MODE_32` and `CS_MODE_64` (`createPpc32` / `createPpc64`, big-endian).

- Integer ALU, word loads/stores, branches, LR/CTR, `rlwinm`, `cmp*` run in
  **both** modes (`ALL_MODES`).
- `ld`/`std`/`ldx`/`stdx` and the `rldicl` family are `ONLY_MODE_64`.
- No tests were deleted, skipped, or `DISABLED_`.

## ABI (what the abi files claim)

### 32-bit SYSV (`AbiPowerpc`, `CC_POWERPC`)

Implemented in `src/bin2llvmir/providers/abi/powerpc.cpp` plus
`PowerPCCallingConvention` (not edited here):

- `r1` stack pointer.
- Integer args `r3`–`r10`; returns `r3`/`r4`.
- FP args `f1`–`f8`; FP return `f1`.
- Linux `sc`: number in `r0`, args `r3`–`r8`, result in `r3`.
- Link register is the hardware `LR` (not a GPR); `mflr`/`mtlr` lift it.

### 64-bit (`AbiPowerpc64`, `CC_POWERPC64`)

Same GPR roles (`r1` SP, `r3`–`r10` args, `r3` return, `LR`, Linux `sc` in
`r0`/`r3`–`r8`). One calling-convention object is used for both ELFv1 and
ELFv2.

## Remaining ABI gaps (honest)

These are **not** modelled. They live in calling-convention code this task
does not own, or require decoder/config work:

- **ELFv1 vs ELFv2.** No function descriptors (ELFv1 `.opd`). No ELFv2 `r12`
  for global-entry/PLT. TOC pointer `r2` is not an ABI-tracked register.
- **64-bit FP args.** `PowerPC64CallingConvention` lists `f1`–`f8`, `f10`,
  `f11` — it skips `f9` and stops short of ELFv2’s `f1`–`f13`.
- **AIX / Darwin / Windows PowerPC.** Not implemented. Syscall layout above
  is Linux.
- **Vararg / homogeneous aggregates / `va_list`.** Not specialised.
- **Vector args** (Altivec `v2`–`v13`, VSX). No CC entries; the lifter
  leaves those insns as pseudo-asm anyway.

## Still pseudo-asm / nullptr (not the production bar)

On both widths: SPE/`ev*`, most SPR moves, cache/TLB, `sc`/`rfi`, traps.
On 64-bit specifically: `divd`/`divdu`, `mulld`/`mulhd`, `sld`/`srd`/`srad`,
`cntlzd`, `popcntd`. Compilers usually spell 64-bit shifts as `rldicl`/
`rldicr` (now lifted), so those nullptr entries are less common in C code
than the MD-form family was.
