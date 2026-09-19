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

## CI

Weekly `sanitizers.yml` runs ASan over `tests/decompile_samples` corpus.
Promote any ASan findings into this directory with `ingest_fuzz_crash.sh`.
