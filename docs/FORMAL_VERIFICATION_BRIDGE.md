# Formal verification bridge

What this tree actually runs, versus analyst paths that are documentation
only. Version **2.0.22**.

In-tree formal verification is **ESBMC on header-only arithmetic kernels**,
not Frama-C, not whole parsers, and not a proof that decompiled C matches the
binary. Register: `C-ESBMC` in [CLAIMS.md](CLAIMS.md). Detail:
[VERIFICATION.md](VERIFICATION.md).

---

## What CI does (ESBMC)

`.github/workflows/verify-esbmc.yml` pins **ESBMC v8.5** (not
`releases/latest`).

| Job | Trigger | Command |
|-----|---------|---------|
| `syntax` | push / path-filtered PR / weekly | `./scripts/verify_esbmc.sh --syntax`; `--routing`; `--doc` (this repo's `docs/VERIFICATION.md` table must match the harnesses) |
| `prove` | after `syntax` | `./scripts/verify_esbmc.sh` — **286** properties across **14** kernels under `include/retdec/utils/` |
| `cross` | weekly + `workflow_dispatch` only | `./scripts/verify_esbmc.sh --cross` — z3 and boolector; not on every push |

`--optional` (including whole-function `PeReader::open`) is **not** in the 286.
On the machine that wrote those harnesses, every backend was OOM-killed. A
green `prove` job is not a proof of `PeReader`.

The kernels are the count/length/offset/index/encoding arithmetic parsers
call. The workflow comment that "ESBMC's models of the C++ standard library
do not stretch to this codebase" is **stale relative to
[VERIFICATION.md](VERIFICATION.md)**: `std::vector` / `std::string` /
classes with methods do verify; `std::unique_ptr` is a parse error; memory
cost is what stops whole-function parser proofs.

Fuzzing (`scripts/standalone_fuzz.sh`, `.github/workflows/fuzz-pr.yml`)
shows a bug **exists**. ESBMC is the complementary claim: these fourteen
headers have no counterexample in the bounded (or loop-free) queries the
harnesses state.

---

## What this is not

| Claim | Status |
|-------|--------|
| Parsers (`PeReader`, ELF, Mach-O, …) are proved | **No.** Optional harness OOMs; unit tests and fuzz remain. |
| Decompiled C is semantically equal to the binary | **No.** CC-01 asks whether emitted C **compiles**. |
| Differential neural gate executes decompiled C | **No.** `C-NEURAL-DIFF` withdrawn. |
| Frama-C / WP / value analysis ships in-tree | **No.** No CMake target, no CI job, no `scripts/frama_c_prepare_stub.sh` in the tree. |
| RISC-V or a C++ HLL writer is in the proved set | **No.** Unimplemented (`C-ARCH-UNIMP`; `C-CXX-EMIT` withdrawn). |

---

## Off-tree analyst path (Frama-C) — documentation only

Analysts who want **partial** evidence on **decompiled** C can round-trip
RetDec output into Frama-C. Treat results as analysis of the **decompiled
model**, not of the original binary. Nothing below is wired into RetDec.

```
  binary.exe
      │
      ▼
  retdec-decompiler -o module.c --select-functions verify_me  binary.exe
      │
      ▼
  module.c  +  module.c.config.json
      │
      ▼  (team-local script — not shipped)
  strip RetDec meta comments, fix types, add ACSL stubs
      │
      ▼
  frama-c -val -main verify_me module_frama.c
  frama-c -wp -rte -main verify_me module_frama.c
```

`--output-lang cpp` is rejected. Prefer default C. RISC-V binaries have no
lifter.

### Limitations

| Topic | Gap |
|-------|-----|
| Memory model | Decompiled pointers may not match original provenance |
| Loops | Structuring may emit `while(1)` with breaks; invariants are manual |
| Inline asm | Lost or opaque |
| Floating point | x87/SSE lowering may not match Frama-C IEEE assumptions |
| Whole program | Selected functions only; no automatic CRT link |

Future hooks (`scripts/retdec_export_frama.py`, GUI “Export for
Verification”, `support/formal/` ACSL templates) are **not shipped**.

---

## References

- [VERIFICATION.md](VERIFICATION.md) — kernels, proof counts, solver pins
- [FUZZING.md](FUZZING.md) — `fuzz-pr.yml` vs `tests/crash_corpus`
- [pipeline_stage_map.md](pipeline_stage_map.md) — Stage 28 output validation
- Frama-C user manual: https://frama-c.com/html/documentation.html
