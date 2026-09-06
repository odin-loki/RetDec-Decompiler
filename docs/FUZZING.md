# Fuzzing the untrusted-input parsers

Everything a decompiler reads is hostile by assumption. `scripts/standalone_fuzz.sh`
builds and runs libFuzzer harnesses for the parsers that read attacker-controlled
bytes, using nothing but `clang++`.

```bash
./scripts/standalone_fuzz.sh --replay        # deterministic regression, ~1 min
./scripts/standalone_fuzz.sh                 # replay, then fuzz each target
./scripts/standalone_fuzz.sh pyc wasm        # only these
./scripts/standalone_fuzz.sh --time 600      # seconds per target
./scripts/standalone_fuzz.sh --list
```

Requires `clang++` **and its sanitizer runtime** — on Debian/Ubuntu that is the
matching `libclang-rt-<N>-dev` package, plus `zlib1g-dev` for the archive
readers. The script says so if the runtime is missing rather than failing at
link time.

## Targets

| Target | Parser | Reads |
|---|---|---|
| `pyc` | `pyc_parser` | CPython bytecode and its marshal stream |
| `lua` | `lua_parser` | Lua bytecode chunks |
| `wasm` | `wasm_parser` | WebAssembly modules |
| `dex` | `dex_parser` | Android DEX |
| `apk` | `dex_parser` | APK archives |
| `jvm` | `jvm_parser` | Java `.class` |
| `jar` | `jvm_parser` | JAR archives |
| `pdb` | `pdbparser` | Microsoft PDB |
| `cil` | `cli_parser` | .NET CIL metadata |

ELF, PE, Mach-O and the unpacker harnesses are **not** here: they link
`retdec::fileformat`, which publicly links LLVM. Those stay on the
`-DRETDEC_FUZZ=ON` path in `.github/workflows/fuzz-pr.yml`.

## The two modes, and why they are separate

**`--replay`** runs every seed and every reproducer committed under
`tests/crash_corpus/<target>/` with `-runs=0`. No mutation, so the result is the
same on every machine and every run. That is what gates a pull request: it
cannot be flaky, and it fails if a previously fixed crash comes back.

**Discovery** (the default) mutates. It may find something new, which means it
may fail for a reason unrelated to the change under test — so it runs on a
schedule, not on pull requests. A find is written to
`tests/crash_corpus/<target>/` via `-artifact_prefix`, and the job uploads it.
**Commit the reproducer**: it turns a one-off find into a permanent regression
case that `--replay` will check forever.

### A caveat about out-of-memory artifacts

There are two kinds of OOM and they behave differently on replay.

A **single oversized allocation** — the parser calling `reserve()` on a count it
read out of the file — is caught by `-malloc_limit_mb`, fires on one execution,
and replays deterministically. That is the class that matters most here, and it
is why replay passes `-malloc_limit_mb=512` rather than relying on the RSS limit
alone. Without it, an OOM reproducer replays clean and the whole class goes
ungated.

An **RSS-limit** OOM is cumulative: libFuzzer notices total memory growth and
saves *the last unit it ran*, which is not necessarily the one responsible. Such
an artifact is a useful corpus seed but not a reproducer, and replaying it
proves nothing. Do not treat a saved artifact as a demonstrated bug until you
have watched it fail on its own:

```bash
./build/fuzz/bin/<target> <artifact> -rss_limit_mb=2048 -malloc_limit_mb=512
```

### `-max_len` is not a tuning knob

libFuzzer defaults to 4096-byte inputs, and that default hides whole bug classes
rather than merely slowing their discovery.

A Python marshal container costs two bytes per nesting level, so 4 KB caps
nesting at about 2000 — comfortably survivable. The stack overflow that a 30 KB
file triggers is unreachable at the default no matter how long the fuzzer runs,
and 1.4 million executions duly reported nothing. Both modes therefore pass
`-max_len=65536`, and replay passes it too so a large committed reproducer is
not silently skipped.

Some things stay out of reach even so: the Python 3.11 line-table accumulator
needs roughly a 34 MB input to overflow. That one is covered by a unit test
instead. Fuzzing is not a substitute for reading the code.

## Seeds

Each target is seeded from its fixtures under
`tests/managed_integration/fixtures/`, plus everything in
`fixtures/malformed/`. The malformed fixtures matter more than the valid ones —
they already sit on the error paths, which is where the bugs are.

The corpus lives in `build/fuzz/corpus/<target>/` and is cached between CI runs,
so coverage accumulates rather than restarting from the seeds every night.

## What it has already found

Within the first few minutes of its first run:

| Finding | Where |
|---|---|
| Out-of-memory: `reserve()` on a tuple count read straight from the file — five bytes can claim 2³¹ elements | `src/pyc_parser/py_marshal.cpp` |
| Crashes in eight of the nine targets on the first four-minute-per-target run; only the CIL parser survived, at 48 million executions | across the parsers |
| Heap-buffer-overflow: the 3.11 line-table decoder checked for one byte and read two, and for two and read three | `src/pyc_parser/py_code_object.cpp` |
| `fuzz_dex.cpp` did not compile — it called a `DexFile::classDefsSize()` that no longer exists | `tests/managed_integration/fuzz/` |
| Stack exhaustion from 15,000 levels of marshal nesting in a 30 KB file — invisible at the default `-max_len` | `src/pyc_parser/py_marshal.cpp` |

That last one is the reason this script exists. The pull-request job could only
check that the harness *files were present*, so a harness that had stopped
compiling looked exactly like a healthy one.

## Writing a new harness

Add `LLVMFuzzerTestOneInput` under `tests/managed_integration/fuzz/`, then a row
in the `TARGETS` table in `scripts/standalone_fuzz.sh`:

```
name : harness stem : src modules : C++ standard : link libs : seed glob
```

The modules must be dependency-free — `scripts/standalone_check.sh --list` shows
which ones are. A harness that needs LLVM belongs in the CMake `RETDEC_FUZZ`
path instead.

The contract is the usual one: **any input at all, no crash.** Rejecting
malformed input is correct behaviour; aborting, over-reading, or allocating what
the file claims is not.
