# Crash corpus (Part 10.3)

Fuzzer crashes are ingested here as permanent regression tests for RetDec
Imortek parsers. This is not a decompilation quality corpus and not a
product sample pack. v2.0.22 installers do not ship these files.

## Layout

One directory per fuzz target, named after the target, holding the inputs that
target must survive. The name is what seeds the replay, so it has to match
`scripts/standalone_fuzz.sh` `TARGETS` (or the libFuzzer job).

Directories that exist today:

```
tests/crash_corpus/
  README.md
  apk/  dex/  jar/  jvm/  lattice/  lua/  pdb/  pelib/  pyc/  wasm/
  demangle/  loadersim/
        # standalone_fuzz.sh TARGETS (dependency-free parsers)
  fuzz_elf/  fuzz_macho/
        # libFuzzer targets in .github/workflows/fuzz-pr.yml; they link
        # retdec::fileformat and cannot run in the standalone path
```

`standalone_fuzz.sh` also lists **cil** and **unpack**. Those names have no
committed subdirectory here until a crash is ingested.

## Ingest a crash

```bash
bash scripts/ingest_fuzz_crash.sh path/to/crash.bin fuzz_elf
```

The second argument is the directory name, which is the target name.

## How these are replayed

Nothing needs a per-file ctest.

```bash
bash scripts/standalone_fuzz.sh --replay
```

That runs every input under the standalone targets' directories.
`.github/workflows/fuzz-pr.yml` seeds `fuzz_elf/` and `fuzz_macho/` into the
corpus it hands libFuzzer — which runs every corpus file before it starts
mutating, so a committed reproducer is replayed first.

## Hand-written entries

Most files here are libFuzzer's own output, named `crash-`, `oom-` or
`slow-unit-` followed by the input's SHA-1. One is not:

`fuzz_elf/handcrafted-dynamic-sh_size-larger-than-file.elf` is a 224-byte
ELF64 with no program headers and one `SHT_DYNAMIC` section header whose
`sh_size` is `0x6d616e79642e0079` -- the value libFuzzer produced when it found
that `ElfFormat::loadDynamicSegmentSection()` passed `sh_size` to ELFIO's
`section::load()`, which allocates that many bytes. `readelf -S` says "Size of
section 1 is larger than the entire file!". It is hand-written rather than
libFuzzer's blob so that what it tests is legible.

`loadersim/oom_elf_rela_sections_sharing_entries` is likewise hand-written. CI
found an out-of-memory in `loadersim` (2211 MB against the 2048 MB limit) but
the reproducer stayed on the runner. This file reaches the same place: a
little-endian ELF32 (first byte `0x06` is the harness's flags) with 800
`SHT_RELA` section headers, each claiming `sh_offset` 0, `sh_size`
`0xFFFFFFFF` and `sh_entsize` 1 over 64 kB that are mostly `0x08`
(`R_386_RELATIVE`). `LoaderSim::applyELFRelocations()` made one `RelocRecord`
per byte per section -- about 1 GB RSS and a single 512 MB `malloc` -- until it
rejected entsizes below the structure's own size and capped the total at the
file size over the smallest entry.

`lattice/elf_section_entsize_smaller_than_header` is hand-written too. CI's
fuzz-pr found a heap-buffer-overflow in `u32le()` under `parseELF()`
(`crash-efbe3486...`, also left on the runner). This 128-byte ELF64 claims
`e_shnum` 2, `e_shentsize` 1 and `e_shoff` two bytes before the end: each
section header passed `inBounds(soff, e_shentsize)` and was then read up to
64 bytes in. `parseELF()` now ignores the section table unless `e_shentsize`
is at least the real header size (40 / 64) and `e_shoff` is inside the file.

## CI

Weekly `sanitizers.yml` runs ASan over `tests/decompile_samples` corpus.
Promote any ASan findings into this directory with `ingest_fuzz_crash.sh`.
