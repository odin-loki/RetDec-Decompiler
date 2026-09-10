# Confirmed audit findings not fixed here

A 14-dimension subsystem audit of this fork produced 217 findings, 129 of them
high or critical. The ones that could be built and tested without LLVM were
fixed on the branch that produced this file; these could not, so they are
written down rather than attempted.

**Why they are not fixed.** A full build needs the LLVM pin from
`cmake/deps.cmake` -- upstream `llvm-project` 23.1.0, not the Avast LLVM-8 fork
this document used to say -- and the environment this audit ran in could not
build it.
Changing `llvmir2hll` or `src/retdec/retdec.cpp` there would mean shipping an
edit to the C emitter that had never been compiled, let alone run against the
216-binary corpus. Everything below is therefore *read and verified against the
source*, but not tested.

Each entry names the file and line, states what is wrong, and says what the fix
is. Verify before acting: this tree moves, and a finding may already be fixed.

**Later sections carry their own reasons.** The paragraph above explains the
original audit's findings, which were behind a build that environment could not
run. Everything under "Found while fixing the reproducibility defects" and after
was found with CI available and is unfixed for a reason stated in the entry --
an output change too large to make without a reproducer, a platform this
environment cannot compile, a failure whose log does not yet say enough.

---

## 1. The 0/216 recompile failure has one cause, and it is three lines

**Closed by 86970d0, by the narrow route.** The pass is no longer run:
`optimizer_manager.cpp` now carries a comment in place of the `run<>` call
naming the three facts in this tree that break the safety argument the old
comment made. The pass itself is untouched and still deletes every
initialiser-less `VarDefStmt` unconditionally, so it must not be re-enabled
without the use check described below; it is correct only for a back end that
emits no declarations, and this tree has only `c_hll_writer`.

**The 0/216 number is now unmeasured, not fixed.** Nothing had ever handed the
emitted C to a compiler -- DET-01 compares two runs, CACHE-05 compares cache on
against cache off, the recovery gate reads detections, and all three treat the
output as text. `scripts/ci/check_emitted_c_compiles.sh` (CC-01) is that
missing measurement and runs in ctest-linux; until its first number is in, the
README's 0/216 is a figure that predates this branch and nobody has re-taken.
Sections 2 and 3 below are the other whole-file failures and are still open, so
a jump to 216/216 was never expected from this alone.

The original entry follows.

---

`src/llvmir2hll/optimizer/optimizers/no_init_var_def_optimizer.cpp:28`

This was the finding worth acting on first. `README.md` reports default `.c`
recompiling **0/216** while the `--buildable` sidecar reports 216/216, and the
whole sidecar exists to work around it.

```cpp
void NoInitVarDefOptimizer::visit(ShPtr<VarDefStmt> stmt) {
    if (stmt->hasInitializer()) {
        visitStmt(stmt->getSuccessor());
        return;
    }
    ShPtr<Statement> stmtSucc(stmt->getSuccessor());
    Statement::removeStatement(stmt);   // <- unconditional
    visitStmt(stmtSucc);
}
```

Every `VarDefStmt` without an initializer is deleted. There is no liveness check
and no use check. The call site in
`src/llvmir2hll/optimizer/optimizer_manager.cpp:263` described one that did not
exist (this comment has since been replaced by the one that removes the call):

```cpp
// NoInitVarDefOptimizer removes VarDefStmt nodes that have no initializer
// and whose variable is not used after the declaration (dead declarations
// left behind after VarDefStmtOptimizer moved the real defs to their uses).
run<NoInitVarDefOptimizer>(m);
```

The second clause of that comment is not implemented. So a local that is
assigned later but declared without an initializer loses its declaration, and
the emitted C reads `v7 = ...;` with no `int64_t v7;` anywhere — `error: 'v7'
undeclared`. That is a whole-translation-unit failure on essentially every
function, which is exactly what `tu_valid_rate: 0.000` in
`results/decompilebench-full.json` records.

The pass is correct for Python, which emits no declarations at all. It is only
wrong for C.

**Two ways to fix it.** The narrow one is to skip the pass for the C writer at
the call site, which is a one-line change and immediately testable against the
corpus. The better one is to implement the use check the comment already claims,
via `ValueAnalysis` / `VarUsesVisitor` — both are already available to the
manager — so dead declarations still get removed and live ones do not.

Either way the regression test is the same: a function whose local is assigned
but never initialised must still emit its declaration.

**What to expect.** This is not a guaranteed jump to 216/216. Sections 2 and 3
below are also whole-file compile failures, and there may be more behind them
that only become visible once this one stops masking everything. But it is the
first blocker, and until it is fixed no measurement of the default `.c` output
is measuring anything else.

---

## 2. Two more whole-file compile failures in the same backend

Both produce output that no C compiler accepts, so each one alone is enough to
keep a binary at `tu_valid = 0`.

**`goto_cfg_optimizer.cpp:361` — gotos with no label.** Pattern A builds a
replacement `IfStmt`, retargets every inbound goto to it, moves the label onto
it, and then discards it. The result is `goto lab_XXXX;` with no `lab_XXXX:`
anywhere: *use of undeclared label*, from gcc and clang alike. Triggered
whenever a goto target is itself an `if (c) goto L;`.

There is a second, independent source of the same symptom at line 354: the pass
clones the moved statements, and `Statement::clone()` does not copy labels.

**`c_hll_writer.cpp:1194` — a label at the end of a block.** `CHLLWriter` emits
nothing at all for `UnreachableStmt`. A labelled unreachable terminator
therefore emits a label with no statement after it: *label at end of compound
statement*, an error in every C standard before C23. Any block reached only by a
goto and ending in `unreachable` — the standard lowering for a noreturn call —
hits this. Emitting `;` or an explicit trap would do.

**Both of the above are fixed** — `a4a1e92` and the commits around it — along
with a third route to the same `goto` symptom: `HLLWriter::getRawGotoLabel()`
invents `generated_N` for a statement with neither a label nor LLVM
basic-block metadata and is called once for the goto and once for the label, so
without memoisation the two got different names. `tests/llvmir2hll` now runs
(`scripts/ci/check_llvmir2hll_tests.sh`), and all three have regression tests
there.

**And a fourth route, which is what run 266 was still reporting.** Pattern A
of the same pass takes the run of statements between the `if` and the goto's
target and makes it the new if-body. It *copied* that run: `buildChain()`
cloned each statement and called `redirectGotosTo()` on the original so the
label and the inbound gotos followed the clone.

That covers the run's own statements and nothing inside them.
`IfStmt::clone()`, `WhileLoopStmt::clone()` and `ForLoopStmt::clone()`
deep-copy their bodies through `Statement::cloneStatements()`, and no `clone()`
carries a label — so every statement nested inside a moved `if` or loop was
dropped along with its container while a goto elsewhere in the function went on
naming the label it had carried. `goto lab_0x112c;` with no `lab_0x112c:`.

The run is now *moved* rather than copied, which has nothing to fix up: every
statement keeps its identity, so its label, the gotos aimed at it and
everything nested inside it come along untouched. Two of the pass's helpers
(`buildChain`, `isForwardReachable`) were dead afterwards, the second of them
already dead before.

Moving exposed a smaller defect underneath. `Statement::transferLabelTo()` and
`transferLabelFrom()` assigned unconditionally, so transferring from a
statement with *no* label erased the label the destination had. The move
redirects the gotos aimed at the discarded `goto L` onto `L` itself, and `L` is
routinely labelled — the fix would have stranded a different goto than the one
it repaired. Both now return early with nothing to transfer. The two existing
tests named "does nothing when the statement has no label" could not see it:
they left the *other* statement unlabelled too, which cannot tell a no-op from
an assignment of the empty string.

### Closed by run 266: the emitted C contradicts the header it asks for

CC-01 (`scripts/ci/check_emitted_c_compiles.sh`) measured 21/24 on its slice
when this was written. One of the three failures was the goto symptom above by
a route the fixes did not cover; the other two were this:

```
ring_buffer-gcc-O3.c:78:12: error: too many arguments to function 'putc'
    | return putc(c, stream, a3);
    ? 7:#include <stdio.h>
```

`HeadersForDeclaredFuncs::getHeaders()` adds `<stdio.h>` because the semantics
recognise `putc` as a library function, and `emitFunctionPrototypesForNonLibraryFuncs()`
therefore emits no prototype of retdec's own — the header is taken as the
truth. The call is then emitted against a signature the ABI recovery invented,
with an extra argument. The file asserts two incompatible things about the same
function.

**The emitter half of this is now fixed.** The entry is kept because the
reasoning it recorded is what the fix followed, and because the corpus effect
is confirmed by CI rather than here.

The blocker recorded first was that `bin2llvmir`, where the extra argument
originates, needs the pinned LLVM and cannot be built or tested in this
environment — so a change could only be validated by a CI round trip. That was
true of the *front end*. It was not true of the fix, which lives entirely in
`llvmir2hll`, and `scripts/ci/check_llvmir2hll_tests.sh` (L2H-01) builds and
runs that in about four minutes locally. Re-reading a stated blocker to see
whether it covers the thing actually proposed is worth doing; this one did not.

The three repairs rejected then are still rejected, for the reasons given:
truncating to what `getNameOfParam` knows breaks `printf`; emitting retdec's
own prototype trades *too many arguments* for *conflicting types*; suppressing
one header does nothing when another function pulls it in.

What settled it is the fourth option the entry named — an arity and a variadic
flag in the semantics layer, `Semantics::getArityOfFunc()`, beside
`getCHeaderFileForFunc()` where this knowledge already lives. The entry also
said "a table is only worth adding if it is right", so the table is **measured
rather than recalled**: `scripts/ci/check_libc_arity.py` compiles a call with
0–8 arguments against the real header for each of the 469 names the semantics
assign one, and reads the arity off which counts the declaration accepts. One
accepted count is a fixed arity; acceptance from N upwards is variadic with N
named parameters; anything else — a function-like macro such as `isnan`, or a
name not declared on the platform — is left out rather than guessed. 454
entries, 15 left out. Fifty-four independently known arities were cross-checked
against the C standard and all agreed.

That script is also ARITY-01, a CI check: the table cannot drift from the
headers unnoticed. It was self-tested four ways — a wrong arity, a missing
entry, an unmeasurable entry, and a wrong variadic flag — and catches each.

`CHLLWriter::visit(CallExpr)` now drops arguments a known, non-variadic
declaration cannot take. Variadic functions and unknown functions are left
alone; `std::nullopt` from the semantics means "no opinion", not "no
parameters".

What is *not* verified here is the corpus effect: running the decompiler over
`ring_buffer-gcc-O3` needs the front end, so whether CC-01's rate rises is for
CI to say. The floor stays at 0.8750 until a run measures otherwise, and
should be raised with the measurement.

**Run 266 said it.** 23/24, rate 0.9583 — both arity failures gone, and the
floor is now that number. The one file left is `generated_shell_sort-gcc-O2`,
`label 'lab_0x112c' used but not defined`: the goto symptom, and the fourth
route to it is the last entry of the section above.

---

## 3. Silent miscompilation, which is worse than a compile failure

These produce C that compiles cleanly and does the wrong thing. Nothing in the
pipeline catches them, because `tu_valid` and `recompile` both pass.

**`if_to_switch_optimizer.cpp:429` — range comparisons become single cases.**
An unbounded `v >= k` / `v > k` else-if chain is converted into a `switch` whose
cases are single values. `v = 7` takes branch A in the binary and branch C in
the emitted source. This is the most common dispatch shape there is.

**`if_to_switch_optimizer.cpp:310` — signedness ignored.** The `Lt`/`Le`
upper-bound patterns do not distinguish the signed from the unsigned compare, so
`if (v < 1) A; else if (v < 2) B; else C;` on a signed control expression becomes
`switch (v) { case 0: A; case 1: B; default: C; }`. Every negative input takes
`C` instead of `A`.

**`copy_propagation_optimizer_ext.cpp:114` — propagation across an if.**
`canPropagateThroughStmts` invokes `ValueAnalysis` with `visitNestedStmts=false`,
so an intervening `if`/`while`/`for` is treated as opaque rather than as
unknown. `t = f(); if (c) { t = 0; } g(t);` becomes
`t = f(); if (c) { t = 0; } g(f());` — the call is duplicated, moved past a
reassignment, and past whatever the body did.

**`while_true_to_for_loop_optimizer_ext.cpp:105` — loop direction by name.**
Whether a loop counts up or down is decided by a substring match on the
variable's *name*. A descending loop whose induction variable is not named
suggestively is emitted with an ascending `i < N` exit test, so the emitted loop
terminates differently from the binary.

---

## 4. The public API cannot report failure

`src/retdec/retdec.cpp:1083` — `decompile()` is declared `bool` and returns
`EXIT_SUCCESS`, which is `0`, which is `false`. There is no failure return path
at all. So the idiomatic `if (!retdec::decompile(cfg)) { /* error */ }` reports
an error on every successful run, and `if (retdec::decompile(cfg))` never fires.

`src/retdec/retdec.cpp:1346` — `parallelBatchDecompile()` consequently reports
every job as failed. A harness batching 216 binaries and counting successes gets
0/216 whatever happens.

`src/llvmir2hll/llvmir2hll.cpp:514` — failing to open the output file exits 0
having written nothing. `retdec-decompiler -o /nonexistent/dir/out.c foo.elf`
succeeds, silently.

`src/retdec-decompiler/retdec-decompiler.cpp:1028` — there is no unknown-option
diagnostic anywhere in the CLI parser. A malformed command line prints help to
stdout and exits 0, and an unrecognised option is taken as the input filename.

These four together mean no script, Makefile or CI job can currently tell
whether a decompilation worked. They are small, independent fixes; they need
the build only because `src/retdec/` and `src/llvmir2hll/` link LLVM.

---

## 5. The SSA the detectors run on is not the SSA they were written against

`src/retdec/llvm_to_ssa.cpp`

This is the widest finding in the file, and it is one defect, not a family of
them. `src/retdec/retdec.cpp:684` builds the module every structural detector
consumes:

```cpp
ssaMod = buildSsaModule(*module);
```

`buildSsaModule` calls `translateInstr` per instruction, and `translateInstr`
populates `IrInstr::uses` in exactly one place:

```cpp
if (instr
    && (llvm::isa<llvm::BinaryOperator>(li) || llvm::isa<llvm::PHINode>(li)
        || llvm::isa<llvm::AtomicRMWInst>(li) || llvm::isa<llvm::AtomicCmpXchgInst>(li)))
{
    for (unsigned i = 0, n = li.getNumOperands(); i < n; ++i)
    {
        const auto* c = llvm::dyn_cast<llvm::ConstantInt>(li.getOperand(i));
        if (!c || c->getBitWidth() > 64) continue;      // <- everything else dropped
        ...
        instr->uses.push_back(u);
    }
}
```

Three things follow, and each was checked by reading the whole file:

1. **`defValue` is never assigned.** The string `defValue` does not appear in
   `llvm_to_ssa.cpp` at all (a grep for it matches only `UndefValue`). Every
   instruction in a production module therefore defines nothing.
2. **Only immediate operands ever become uses**, and only on four instruction
   classes. A register operand — the entire point of a def-use graph — is
   skipped by the `continue`.
3. **`Load` and `Store` are not in that list**, so they arrive with empty use
   lists. Not "partially populated": empty.

No `SSAPass` runs on the result either; `retdec.cpp` never calls one. And it
could not help if it did, because renaming needs value operands to rename and
there are none.

**What this costs.** Any predicate that reads `instr->defValue`, follows a
non-immediate use, or relates two instructions by value is dead on the
production path — it cannot return true for any binary, however well the code
matches. Confirmed dead by this route, at least:

| File | Predicate | Needs |
|---|---|---|
| `algo_recover/binary_search_detect.cpp:379` | the whole detector | `defValue` |
| `algo_recover/find_detect.cpp:88` | `hasImmediateComparand` | `defValue` |
| `container_detect/list_detect.cpp` | `hasSentinelInit` | `Store::uses[0..1]` |
| `container_detect/map_detect.cpp` | `hasRotation` | `Load::defValue`, `Store::uses[0..1]` |

Their unit tests pass because the fixtures hand-build IR with `defValue` set and
real operand lists — which is what the detectors were designed against, and what
the adapter does not deliver.

Two consequences worth stating plainly, because they change how other findings
should be read:

- The *precision* fixes made to these detectors on this branch are real and
  worth keeping, but their corpus impact is nil until the adapter is fixed:
  a predicate that cannot fire cannot fire wrongly either.
- Conversely, any claim that a detector's false positives explain part of the
  0.056 name-blind F1 is unearned for the detectors above. They contribute
  nothing on real input, in either direction. That is its own kind of bad.

**The fix.** Give `translateInstr` a `DenseMap<const llvm::Value*, ssa::ValueId>`
memo, allocate an `IrValue` per LLVM `Value` on first sight, set
`instr->defValue` for every instruction that produces one, and push a `Use` for
every operand rather than only `ConstantInt` ones — with `Load` and `Store` in
the set, and the `Store` operand order (value, address) that the detectors
already document. It is a self-contained change to one file, but it needs LLVM
to compile and the 216-binary corpus to show it helped, so it is not attempted
here.

Worth doing before any further detector tuning: the tuning is currently being
measured against fixtures rather than against anything the pipeline produces.

---

## 6. Other confirmed items

~~`retdec-decompiler` aborts on `generated_quicksort-gcc-O0`~~ — diagnosed and
fixed, and the guess recorded here was wrong.

This entry read: "The four candidates are in
`src/bin2llvmir/analyses/reaching_definitions.cpp` (lines 248, 277, 343 and
347), all of the shape 'we should have this basic block in bbMap'." None of
them. The assertion is in LLVM, and the gate had never printed it: the
prediction extractor kept the last three lines of the crash, and an LLVM
assertion prints its message FIRST. With `summarise_failure_output` keeping
both ends, `ctest-linux` run 34271470725 said:

```
llvm/lib/IR/Value.cpp:523: void llvm::Value::doRAUW(llvm::Value*, ReplaceMetadataUses):
Assertion `New->getType() == getType() &&
          "replaceAllUses of value with new value of different type!"' failed.
1.  Running pass 'LLVM instruction optimization using RDA' on module ''
```

`ReachingDefinitionsAnalysis` pairs a definition with a use by their POINTER
operand — `initializeBasicBlocks` records `Definition(s, s->getPointerOperand())`
and `Use(l, l->getPointerOperand())` — and says nothing about the value types at
either end. Under opaque pointers a store of an `i32` and a load of an `i64`
through the same register global are one definition and one use of it, and
`inst_opt_rda.cpp` called `replaceAllUsesWith` across them. The three patterns
in `inst_opt_rda_ext.cpp` each check the types already; the two in
`inst_opt_rda.cpp` predate opaque pointers, where a pointer's element type tied
the two together, and did not. Both check now.

Not reproducible in this environment either way — it needs the pinned LLVM
build — so the fix rides on the next `ctest-linux` run rather than on a local
test.

Eleven of the thirteen test sources in `tests/fileformat/` and one in
`tests/common/` are named by no `CMakeLists.txt`, so `ctest` does not build or
run them: `ar_archive_format_probe_tests.cpp`, `coff_format_tests.cpp`,
`elf_format_tests.cpp`, `format_detection_tests.cpp`,
`format_factory_tests.cpp`, `intel_hex_format_20bit_tests.cpp`,
`intel_hex_format_tests.cpp`, `intel_hex_token_test.cpp`,
`macho_format_tests.cpp`, `pe_format_tests.cpp`, `raw_data_format_tests.cpp`
and `tests/common/calling_convention_tests.cpp`. `tests/fileformat` builds two
files. Whether they still compile against the LLVM 23.1.0 migration is not
known here, because everything they link needs that build; re-enabling them
blind would break `ctest-linux` for hours per attempt, so they are listed
rather than switched on. `scripts/check_cmake_sources.sh` now fails on any
*new* test source that nothing builds, and carries these twelve in an explicit
`UNBUILT` list with that reason attached, so the set cannot grow quietly.

`src/bin2llvmir/providers/` — the ten provider maps are process-wide, and
`ProviderInitialization::runOnModule` starts by calling `clear()` on all of
them. `clear()` drops everyone's entries, not the caller's, so a second pipeline
destroys the `Config` and `FileImage` the first is still holding pointers into.
`parallelBatchDecompile()` ran N pipelines on a thread pool, which made that a
use-after-free on the fast path of an API that advertises parallelism.

Not fixed here, because the fix is ten headers and every accessor in them --
key and clear per `llvm::Module` rather than per process -- and nothing in this
environment can compile bin2llvmir to check it. What *is* done is that
`decompile()`, `decompileToLlvmIr()`, `tryEmulationUnpacking()` and
`disassemble()` now take a process-wide recursive lock, so the pipelines take
turns instead of corrupting each other. `parallelBatchDecompile` is therefore
correct and serial; `retdec.h` and `docs/PERFORMANCE.md` say so. Lifting the
lock is what the provider refactor is for.

The two mutable statics in `ModulePassPrinter` that the same batch path raced --
`LastPhase`, a `std::string` every pass assigned to, and
`passWallStartForTimedPass` -- are `thread_local` now. They are per-pipeline
state and there is one pipeline per thread.

`src/neural/llama_inference.cpp:242` — `llama_tokenize` is called with
`parse_special=false`, so the ChatML template `prompts.cpp` builds is fed to the
model as literal text and never applied. Every non-mock refinement runs the
model outside its instruction-tuned distribution.

~~`src/retdec/semantic_recovery_export.cpp:95` — semantic comments are placed by
DWARF/PDB *source* line number~~ — fixed. `definitionLineOf` finds the function
in the emitted output by name, using the same declarator tests the sidecar
generator uses, so a stripped binary gets its comments and a binary with debug
info gets them in the right place. Four tests, six of which fail against the
old placement.

~~`src/retdec/semantic_recovery_export.cpp:1583` — comment injection and sidecar
generation both run unguarded on the output file~~ — fixed. `-f json` and
`-f json-human` route llvmir2hll through a `JsonOutputManager`, so both now
refuse: `outputIsSourceListing` gates the injector, and
`maybeWriteBuildableSidecars` gained a config overload that does the same. The
same read-and-rewrite also dropped the file's trailing newline whether or not a
comment was added; it now leaves an untouched output byte for byte.

`src/retdec-decompiler/retdec-decompiler.cpp:891` — managed inputs
(`.apk`/`.jar`/`.pyc`/`.class`/`.wasm`) are written to a `.c` filename and
bypass `--silent`, `-f json` and log redirection entirely.

---

## Working on these

The three fast checks on this branch do not cover any of this code — they are
LLVM-free by construction, and all of it is behind LLVM. Use the real build:

```bash
cmake --preset core-release
cmake --build build/linux -j"$(nproc)"
ctest --test-dir build/linux --output-on-failure
```

For section 1, the measurement that matters is the corpus recompile rate:
`results/decompilebench-full.json`, regenerated per `docs/BENCHMARKS.md`. If
`tu_valid_rate` moves off 0.000, the fix landed.

---

# Residual findings from the SMT/adversarial-verification pass

A separate exercise from the audit above: thirteen arithmetic kernels were
proved with ESBMC, the call sites were routed through them, and every round of
fixes was then handed to an adversarial verifier whose brief was to disbelieve
it. Four rounds ran.

**Adversarial verification does not terminate.** Each round found fewer defects
in the fixes and more sites of the same class elsewhere; a fifth round would
find a sixth. What follows is the state at the point where that was called,
written down so the call is visible.

Everything in the previous version of this section that was reachable in this
environment has since been fixed — the second unbounded `descriptorToType`, the
LocalVariableTable extent in two more modules, three DEX conversions,
`set10Byte`'s un-inverted path, `nibblesFor`'s capacity, the divergent match
loop, and the stale citations. What is left is here.

## Behind a build this environment cannot run

Everything below is compiled only by the LLVM-dependent build, so a change to it
here would ship untested — the same reason the audit findings above were written
down rather than attempted. All were read and measured against the source; none
were compiled.

* `src/fileformat/file_format/elf/elf_format.cpp:2770-2773` and
  `coff_format.cpp:600-609` — `getDeclaredFileLength` overrides that shadow the
  base function which *was* fixed, each forming unclamped 64-bit sums (and, in
  the COFF case, an unchecked multiply) of header fields.
* `src/fileformat/file_format/file_format.cpp:2309` — `getXByte` still forms
  `secSeg->getOffset() + secOffset`.
* `file_format.cpp:583-593` (`computeSectionTableHashes`) and `:1380`
  (`getOverlayEntropy`) — unchecked multiply and sums on header fields.
* `src/fileformat/utils/other.cpp:407` — `getRealSizeInRegion`'s wrapping clamp.
  Re-measured: `(10, 2^64-5, 512)` returns `18446744073709551611`. Reached from
  `sec_seg.cpp:235/402` and `resource.cpp:93/218`.
* `src/fileformat/types/resource_table/bitmap_image.cpp:210, 302, 383, 437, 490`
  — unchecked `nBytesInRow * nRows` from header width/height/bitCount, feeding
  the clamp above.
* `src/loader/loader/segment.cpp:205`, `segment_data_source.cpp:81-82, 91` —
  post-hoc and pre-copy wrapping clamps. `Image::getXBytes` now refuses before
  them, but both have other callers.
* `src/cli_parser/cli_sig.cpp` — a self-referential TypeSpec whose signature is
  a GENERICINST with two or more self-referencing type arguments (e.g.
  `15 12 06 02 12 06 12 06`) drives an exponentially wide traversal under the
  depth-64 bound. The bound stops the depth, not the width. Reachable, but the
  fix is a visited-set through `ITypeNameResolver`, whose other implementation
  is behind the LLVM build.

## Structural: code no test can reach

**Mostly closed.** `src/utils/gpu_scanner.cu`'s CUDA half -- roughly 660 lines,
all of the device code and all of the runtime plumbing -- was compiled by
nothing in this tree, because `src/utils/CMakeLists.txt` selects it only when
CUDA is found and `gpu_scanner_cpu.cpp` includes it with
`RETDEC_GPU_SCANNER_HOST_ONLY` defined. An adversarial verify pass proved the
consequence by reverting four fixes inside that region and watching the suite
stay green at 13/13.

`tests/utils/cuda_stub/` is now enough of the CUDA runtime to compile it with an
ordinary C++17 compiler, and `scripts/standalone_check.sh` does so on every run.
It caught a real defect the first time it ran. What the stub provides is
documented at the top of `cuda_runtime.h`, including what it does *not* model:
no warp semantics, no coalescing or timing, and blocks serialised rather than
concurrent -- so a kernel with an inter-block race would pass there and fail on
a device.

What is still open is one step further:

* The CUDA half is **compiled but not executed** by the suite. The stub can run
  a kernel -- `__syncthreads()` is a real barrier across `std::thread` lanes, so
  a kernel that fills a `__shared__` array before the barrier and reads its
  neighbours' entries after it behaves correctly -- but nothing links it,
  because `GpuScanner`'s methods are already defined by `gpu_scanner_cpu.o` and
  the two cannot go in one binary. Closing it means either a second test binary
  that links the `.cu` instead of the `.cpp`, or splitting `GpuScanner` so the
  device path is a separate type.

### `src/opencl/` and its eight test suites are built by nothing

`src/opencl/` is 4,529 lines of OpenCL host code — context, disk cache,
profiler, buffer pool, parallel disassembler, type inferencer, Steensgaard
analysis, semantic hasher, e-graph simplifier — with a complete
`CMakeLists.txt` that no `add_subdirectory` anywhere in the tree names.
`tests/opencl/` has eight suites and a complete `CMakeLists.txt` that
`tests/CMakeLists.txt` likewise never adds. Neither the library nor its tests
is compiled by any build of this repository, and
`cmake/superbuild/stubs/retdec-ocl/` describes itself as the stub for a "real
implementation in `src/opencl/`" that nothing links.

Found by `scripts/check_cmake_sources.sh` once it was taught that a directory
carrying a working `CMakeLists.txt` that nothing includes is the same failure
as a source no `CMakeLists.txt` names.

Not wired in here, and the reason is specific rather than general:
`src/opencl/CMakeLists.txt` opens with `find_package(OpenCL REQUIRED)`, so
adding it unconditionally fails configure on every machine without an OpenCL
SDK — it needs an option and a guard, and the guard has to be exercised in both
states. No configure of this tree can run in this environment (it fetches and
builds LLVM), so wiring a 4,529-line library into the build here would be an
unverified change to the build of every consumer. `tests/opencl` carries that
reason in `UNBUILT_DIRS` in `scripts/check_cmake_sources.sh`, so the check
passes for a stated reason rather than because it cannot see the directory.

Closing it means: an `RETDEC_ENABLE_OPENCL` option defaulting OFF,
`add_subdirectory(opencl)` in `src/CMakeLists.txt` and `tests/CMakeLists.txt`
behind it, and a CI job that configures with it ON against a POCL or Mesa
Rusticl ICD — then the `UNBUILT_DIRS` entry comes out.









## The Imortek codegen wrote every `goto` with no label to match

`src/codegen/emitter.cpp` emits `goto L<blockId>;` for a
`StructNode::Kind::Goto` — which is what `CompilerStructurer` produces for
every irreducible region and every back edge it cannot fold. Nothing emitted
the label. `CStmt::labelStmt()` exists, `Emitter::emitStmt` renders
`Kind::Label`, `GotoEliminator` reads `Kind::Label` statements — and the only
callers of `labelStmt()` anywhere in the tree are five lines of
`tests/codegen/codegen_test.cpp`.

So the back end could not emit a `goto` and a working translation unit at the
same time. One undefined label is not one bad function: the C compiler
rejects the file, and every other function in it goes with it.

`gotoLabelsFor()` now walks the `StructNode` tree once, pairs each `Goto`
node with the block it names, and the `Block` case emits that label. Two
gotos to one block agree on the spelling because the label is looked up from
the same map that the goto is.

A goto naming a block the tree has no `Block` node for is dropped rather than
emitted: a label can only go on a block, so such a goto has nowhere to land,
and a wrong function is less destructive than a file no compiler will read.
The structurer only emits a goto to a block it has already visited, so this
guard is on a shape it does not currently produce — it has a test because a
guard nothing exercises is indistinguishable from one that does not work.

Both halves verified by reverting them one at a time.

## The whole back end, from LLVM IR, in one test

Every other test under `tests/llvmir2hll` exercises one pass on a module
built by hand. What the decompiler actually does — convert, then run thirty
passes in order, each on the output of the last — was covered by nothing but
the corpus, which needs the front end and a CI round trip.

`OptimizerManagerPipelineTests` takes LLVM IR, converts it, runs
`OptimizerManager` over the result, and asks whether every goto still has a
target the emitter reaches. Three shapes so far: a multi-exit loop, the
nested multi-exit shape read off `generated_shell_sort-gcc-O2`'s emitted C,
and an irreducible two-block region. All three hold, so none of them is the
shape that loses `lab_0x112c`.

The harness takes a `disabledOpts` set, which is the point: once a shape does
reproduce, bisecting the pipeline names the pass in a few local runs rather
than a few CI rounds.

What is missing is the real input. `--save-failures` now keeps the `.ll` the
back end was given alongside the `.c` it produced — the decompiler writes it
beside its output and only removes it with `--cleanup`, which CC-01 does not
pass. One CI round hands over the exact IR for the function that fails, and
that IR goes straight into this harness.

## 384 more tests, and the pass that decides what becomes a `goto`

`scripts/ci/check_llvmir2hll_tests.sh` excluded
`src/llvmir2hll/llvm/llvmir2bir_converter/` and `tests/llvmir2hll/llvm/`
outright, with a reason recorded: they use LLVM APIs that moved between the
system LLVM and the pinned one, naming
`ConstantPointerNull::getPointerType` and the `LoadInst` constructor overload
set.

Both were true. Neither justified the exclusion. Compiling the twenty sources
and nineteen test files one at a time against LLVM 18 found **one**
production call and **two** calls in one test file that did not build:

* `cNullPtr->getPointerType()` → `cNullPtr->getType()`. `ConstantPointerNull`
  specialises `getType()` to return a `PointerType*` in every LLVM this code
  has been built against; `getPointerType()` went with typed pointers.
* `new llvm::LoadInst(ty, p, "", false, Align(1), nullptr)` — `nullptr`
  matches both the `Instruction *InsertBefore` and `BasicBlock *InsertAtEnd`
  overloads. Dropping the argument takes the `InsertBefore` default and is
  unambiguous everywhere.

Three lines. What they were keeping out included the nine hundred lines of
`structure_converter.cpp`, which is where retdec decides what becomes a
`goto` — the component behind the one CC-01 failure that three rounds of
reading pass sources had not found. The suite goes from 1,793 tests to
2,179, and the floor from 1,500 to 2,000.

This is the third time on this branch that a recorded reason for not running
something did not survive being checked: `tests/cpdetect` and `tests/loader`
were said to need the pinned LLVM and did not, `tests/unpacker` turned out to
need YARA rather than LLVM, and now this. Writing the reason down is what
makes it checkable; checking it is a separate act.

The converter's own goto handling now has a regression test asserting the
property that matters — every goto the emitter reaches has a target it also
reaches — on a multi-exit loop and on the nested multi-exit shape
`generated_shell_sort-gcc-O2` actually has. Both hold, so the front end is
not where `lab_0x112c` is lost; the next place to look is the optimizer
pipeline that runs after it, which this check can now drive end to end.

## Three fields that could only ever hold their default

A sweep of every scalar field declared under `include/retdec` for one that
nothing anywhere assigns turned up 357 candidates, most of them options a
caller sets and a few of them parse artefacts of the sweep itself. Three
survived reading:

* **`ReplacementNode::countReg`** was read and never written.
  `debugStr()` renders a `Memset`/`Memcpy`/`Memmove` with a negative
  `countImm` as `memset(r3, 0, r0)` — where the `r0` is this field, always
  zero. The only matcher that produces these nodes recognises unrolled vector
  copies, whose length is the number of stores times their width and is
  therefore always constant, so `countImm` is always set and the register
  half of the pair never carried anything. An unknown length now prints as
  unknown, and the file comment that promised "countImm or countReg" — and a
  `fillReg` that does not exist at all — says what the matcher actually
  emits.

* **`WatEmitter::DisState::paramCount`** was written once, from
  `mod.types[typeIdx].params.size()`, under a comment reading "Count params",
  and read by nothing. `localId()` resolves a local by index against the name
  section and does not need it.

* **`WatEmitter::DisState::labelCounter`** was neither written nor read.

All three are gone. The sweep is not a gate: at 357 candidates against 2,755
fields the accepted list would be larger than the signal, and the ones that
matter are found by asking whether a field is *read* — which is what OPT-01
already does for options.

## Three merges that drop a body drop the labels inside it

`IfStructureOptimizer::tryOptimization4` and patterns 6 and 7 of
`if_structure_optimizer_ext.cpp` all merge two `if` statements with
*identical* bodies into one `if (A || B)`. Identical means
`Statement::areEqualStatements` — equal by value, two distinct objects — so
one of the two bodies is kept and the other is dropped on the floor, along
with anything nested inside it.

`Statement::clone()` carries no label and neither does this: a label on a
statement inside the discarded body goes with it, while the goto elsewhere in
the function that names it stays. `goto lab_x;` with no `lab_x:` anywhere,
which is the same symptom as `GotoCFGOptimizer` pattern A and the same shape
of cause.

The remedy is the one already applied in the same codebase:
`DeadCodeOptimizer` asks `GotoTargetAnalysis::hasGotoTargets(body)` before it
discards a clause body, six times, and `unreachable_code_in_cfg_remover.cpp`
asks it once. These three now ask it too, each about whichever of the two
bodies it is the one to drop — pattern 7 drops the leading `if`'s body rather
than the second's, so its guard is on the other half.

Each verified by reverting that guard alone. Whether any of the three is what
CC-01 reports on `generated_shell_sort-gcc-O2` is **not** established: three
rounds of reading pass sources have produced three real defects with real
fixes and the same error at the same line 120 each time. The next round reads
the emitted C itself, which `--save-failures` now keeps.

## A workflow that does not parse does not fail — it does not run

`workflow_dispatch` on `ctest-linux.yml` came back with

    failed to parse workflow: (Line: 285, Col: 11):
    'if-no-files-found' is already defined

The file was already pushed. Nothing in this repository read these files, and
there are twenty of them. What an unparseable one costs depends on the
trigger, and the correction below records what was actually observed rather
than what the commit message assumed.

The part worth recording is *how* it got past a check. The file was validated
before the push with `yaml.safe_load()`, which accepts a repeated key and
keeps the last one. It loaded cleanly here and was rejected there. A checker
more permissive than the thing it stands in for is not a checker.

`scripts/ci/check_workflow_yaml.py` (WF-01) parses every workflow with a
loader that refuses duplicate keys at any level, and checks the structure
GitHub requires: a name, a trigger, at least one job, `runs-on` and steps on
every job that is not a reusable-workflow call, and exactly one of `uses` or
`run` on every step. Eleven self-test cases, including the exact shape that
got through — with an assertion in the test that `safe_load` does accept it,
so the reason this check exists cannot quietly stop being true.

Verified against the broken file itself: on the commit as pushed it fails and
names line 285, the same line GitHub named.

## Every condition reached the C writer as a subtraction

`src/retdec/llvm_to_ssa.cpp` mapped both `icmp` and `fcmp` to
`ssa::IrInstr::Op::Compare` and kept nothing else. LLVM names the question a
comparison asks — `slt`, `eq`, `uge` — and the adapter dropped the name.

Downstream, `ExprCoalescer` had nothing to work from, so `Op::Compare` became
`lhs - rhs`: the value a machine `CMP` conceptually computes. Nothing else in
`src/codegen/` constructs a comparison `BinOpKind` either, so **no expression
the C back end ever built contained `<`, `<=`, `>`, `>=`, `==` or `!=`.**
`if (a < b)` came out as `if (a - b)`, and a condition that reached the writer
through a flag read came out as a bare `v37`.

That also made `CondNormaliser` — a 165-line file with nine unit tests, whose
entire job is tidying comparisons — unable to match anything on the emitter
path. Its tests passed because they hand it comparisons directly. Nothing in
the pipeline ever did.

`ssa::CmpPred` now rides on `Op::Compare`, the adapter sets it from the LLVM
predicate, and the coalescer emits the matching C operator. Unsigned
predicates get their operands cast to the unsigned type of the operand width,
because C puts the signedness in the operands and not in the operator, so
`icmp ult` is `(uint32_t)a < (uint32_t)b` and never a signed `a < b` on the
same bits.

Two cases deliberately stay as they were:

* **Unordered float predicates**, and `ORD`/`UNO`. No C comparison operator
  asks whether an operand is NaN, and approximating one with an operator that
  differs exactly there is worse than the subtraction.
* **A machine `CMP`/`TEST`**, which asks nothing on its own: the condition is
  in the branch that reads the flags. `IrInstr::specificFlag` records *which*
  flag a `Jcc` reads and nothing records the polarity, so `je` and `jne` are
  indistinguishable in the IR. Recovering those needs a field the lifter would
  set, and the lifter is in the front end this environment cannot build. Until
  then a binary lifted straight to the SSA IR keeps `a - b`; one that goes
  through LLVM gets the operator.

### The counters beside it, and the knobs above it

`CodeGenPass::Stats::condRewrites` and `::castsRemoved` are documented as
counts of what those two passes did and were assigned by nothing, so they read
`0` on every unit — the same shape as `DVSA::Result::carvedAccesses`. Both are
now threaded through and counted, and `condRewrites` is non-zero end to end
now that there are comparisons to rewrite.

`Config::enableCondNorm` and `Config::enablePtrSyntax` sat beside
`enableGotoElim` and `enableCoalescing`, which are both consulted, and were
read by nothing: the two passes ran whatever the config said. They are
consulted now, and OPT-01's accepted list of inert knobs is down from 69 to
67.

**Still open**: `castsRemoved` reads 0 through the pipeline even so.
`PointerSyntax::recover()` is called on exactly one expression — the
destination of a `Store` — and nothing in `src/codegen/` builds a `Cast` node
that reaches it. `minimiseCasts()` is correct and unreached. Closing it means
running pointer-syntax recovery over expressions generally, which is a
behaviour change to every emitted statement and wants its own measurement.

## Two invariants in `Statement` that nothing was holding

Neither of these had a corpus symptom to point at, which is why they are
recorded here rather than claimed as a fix for one. Both are wrong answers
from a public API, reproduced at that level.

**`setSuccessor()` never took the replaced edge off the record.**

```cpp
if (succ) {
    // Update the predecessors of the old successor.
    succ->preds.erase(succ);
}
```

The comment says what was meant; the line erases the old successor from its
*own* predecessor set, a no-op for any statement that is not its own
predecessor. So after `a->setSuccessor(b); a->setSuccessor(c);`, `b` still
records `a` as reaching it, and nothing cleans it up later:
`removePredecessors(true)` keeps a predecessor whose successor is no longer
this statement, which is exactly the stale one.

Most readers of `preds` are guarded — `isGotoTarget()`, `redirectGotosTo()`
and `removeStatement()` all re-check `getTarget() == this` — but
`getUniquePredecessor()` is not, and `pre_while_true_loop_conv_optimizer`
walks backwards through it and rewrites around the answer. A statement that
does not flow into another is not its unique predecessor. Three tests.

**`removeStatement()` sent a goto's label and its jumps to two different
places.** A goto never falls through, so a jump into `X: goto L;` means L.
The function agreed where the label was concerned — it moves a goto's label
onto the goto's target — and then sent the inbound gotos to the goto's
*successor*, the code the jump existed to skip, and copied the label there as
well. Two statements carrying one label, and the jumps resolving through
neither of them reliably.

It is latent today: every caller that removes a goto pre-empts it.
`GotoStmtOptimizer` prepends the replacement first, and `prependStatement()`
ends in `notifyObservers(stmt)`, which is how an inbound goto gets redirected
onto the prepended statement — so by the time `removeStatement()` runs there
is nothing left in `preds` for it to get wrong. `GotoCFGOptimizer` pattern B
removes a goto only when it is not a goto target; pattern D transfers the
label and redirects the gotos first. An end-to-end test through
`GotoStmtOptimizer` was written and then dropped, because it passed with the
fix reverted: the prepend had already done the redirect. Four tests at the
API level instead, one of them the control for an ordinary statement, which
still falls through to its successor.

## Corrections to claims made in this branch's commit messages

### "A workflow that does not parse does not run" — half right

`9e54dd8`'s message says an unparseable workflow "produces no run at all, so
there is no red X anywhere". That is what `workflow_dispatch` does — the API
call is rejected with the parse error and no run is created — and it is what
the message generalised from.

The push trigger behaves differently, and run 268 is the evidence: the push
of `e69d0b0` created a run that failed immediately, `created_at`,
`run_started_at` and `updated_at` all equal, listed under the workflow's
*path* rather than its name because GitHub had no name to read. So a push
does leave a red X.

What does not change is the reason WF-01 exists: nothing in this repository
looked at these files before the push, and the YAML check that ran did not
reject a duplicate key. Whether the cost is a failed run or no run at all,
the file should not have reached the branch.


### The Dalvik try-region wrap was not reaching a consumer

Commit `12ab168` fixed the uint32 wrap in `DexLifter::wireExceptions`
(`t.startAddr + t.insnCount`) and said the bad numbers "reach every consumer
that asks how long the region is", quoting the comment
`src/jvm_parser/jvm_lifter.cpp` carries about its own equivalent.

That sentence was borrowed rather than checked, and it is wrong for this tree.
Every reader of `BcExceptionHandler::startOffset`/`endOffset` is one of four:

* `src/jvm_reconstruct/stack_sim.cpp` and `src/jvm_reconstruct/local_rebuild.cpp`
  read only `handlerBlock` and `catchType`, never the offsets;
* `src/cil_reconstruct/cil_reconstructor.cpp` uses the pair as a grouping key,
  where a wrapped pair groups as consistently as a sane one — and that is the
  CIL path, not the DEX one;
* `src/java_emitter/java_stmt_emitter.cpp` is the only place a DEX-lifted
  region is read as a range, and it opens with
  `if (eh.endOffset <= eh.startOffset) continue;`.

That guard is total for this input, not lucky: `TryItem::insnCount` is a
`uint16_t`, so the sum wraps only for `startAddr >= 0xFFFF0001`, and then
`end < startAddr` always. Every wrapped region is inverted, and inverted
regions are exactly what line 125 already refuses. Nothing in the tree computes
`endOffset - startOffset` at all.

So the fix is defence in depth and consistency with `protectedRegionFits` on
the JVM side — refusing malformed input where it is read rather than relying on
a downstream guard — and not the live defect the commit message described. The
`catchAllAddrs` out-of-bounds index fixed in the same commit is unaffected by
this: that one segfaults, and the test for it exits 139 without the fix.

---

## Found while fixing the reproducibility defects, and deliberately left alone

### The def-use chain comparator is not a strict weak ordering either

`src/llvmir2hll/optimizer/optimizers/copy_propagation_optimizer.cpp`,
`ordered(const DefUseChains::DefUseChain &)`

`GlobalVarsSorter`'s comparator was rewritten because it was not an ordering
(see `REPRODUCIBILITY.md`). The def-use chain comparator in the same family
has the same defect, and is not being changed.

Its first five steps compare a tuple of totally ordered keys -- number of
uses, variable name, statement text, the set of use texts -- and that part is
a valid ordering. The tail is not. It walks both statements' predecessor
chains in step, comparing `(number of predecessors, first predecessor's text)`
at each level, and stops as soon as *either* chain reaches a statement with no
predecessors:

```cpp
} else {
    break;                 // one chain ran out; the two are "equal"
}
```

That makes a chain equal to any chain it is a prefix of, and prefix-equality
is not a transitive equivalence. Take chains `x`, `xy` and `xz`: `x` compares
equal to `xy` and equal to `xz`, but `xy` and `xz` differ at their second
element, so they do not compare equal to each other. `std::sort` has no
defined behaviour on a comparator like that -- it may read past the end of the
range.

Reaching it needs two distinct `(statement, variable)` entries that tie
through all five earlier steps: same number of uses, same variable name, same
statement text, same use-set texts, same parent and successor texts. The same
assignment on two arms of an `if`, surrounded identically, would do it.

**Why it is not fixed here.** The obvious repair is to end the comparison on
the statement's position in the function's CFG, the way `ordered(StmtSet)`
now does -- `stmtOrder` is already a member and already built for every
`performOptimization()`. But the chain walk currently *decides* comparisons,
not just ties, so replacing it changes the order def-use chains are processed
in, which changes which copy propagations happen first, which changes the
emitted C for binaries that are not misbehaving today. That is a real risk to
take without a binary that reproduces the defect, and there is no such binary
yet: `ordered(du)`'s input is a `std::vector` built by walking the CFG's node
vector in order, so the sort's *result* is deterministic on a deterministic
input even with a bad comparator. The exposure is the undefined behaviour, not
a wrong answer.

**What would settle it.** Run the corpus under a build that checks comparator
validity -- libstdc++'s `_GLIBCXX_DEBUG` diagnoses "comparison doesn't meet
irreflexive requirements" -- and see whether any binary trips it. If one does,
it is worth the output churn; DET-01 will show exactly how much churn.

### The sanitizers workflow never passed, and the reason was in its own options

`.github/workflows/sanitizers.yml`

**Answered and fixed; kept here because the answer is worth having written
down.** Runs 42 through 45 all failed and run 44's commit message says why:
"The sanitizers workflow has never got past the build." Run 45 was the first
that did, and it died in the ASan run itself with one line and no address in
it:

```
ERROR: Failed to mmap
```

The hypothesis recorded here first was that the deliberately `-no-pie` image
loads at `0x400000` and grows up into ASan's low shadow at `0x7fff8000`. That
was wrong. `ASAN_OPTIONS=verbosity=1`, added to make the next run say
something, said it:

```
|| [0x00008fff7000, 0x02008fff6fff] || ShadowGap ||
protect_shadow_gap=0: not protecting shadow gap, allocating gap's shadow
|| [0x000091ff6000, 0x004091ff6fff] || ShadowGap's shadow ||
...
ERROR: out of memory: failed to allocate 0x1000 (4096) bytes of
       InternalMmapVector (error code: 12)
```

`protect_shadow_gap=0` was in the workflow's `ASAN_OPTIONS`. It tells ASan
not to protect the shadow gap but to allocate shadow *for* it -- and the gap
spans 128 TB, so its shadow is about 63 TiB. The mapping goes through under
`vm.overcommit_memory=1`, and then the process cannot get another four
kilobytes: ENOMEM on a 4096-byte `InternalMmapVector`. The option exists for
running under an emulator that cannot protect the gap. Nothing here needs it.

Two things to take from it. The kernel knobs the workflow spends a step on --
`randomize_va_space=0`, `mmap_rnd_bits=28`, `overcommit_memory=1` -- were all
applied and all irrelevant; the answer was an option the workflow set itself.
And a gate that fails with one line and no address in it cannot be diagnosed
at all: the one-line `verbosity=1` is what turned three failed runs of
guessing into a five-minute read.

### The Windows build cannot compile two parsers, because nothing gives it zlib

`src/jvm_parser/CMakeLists.txt:19`, `src/dex_parser/CMakeLists.txt:15`

`ctest-windows` fails on `main`, eleven minutes in:

```
FAILED: src/jvm_parser/CMakeFiles/retdec-jvm-parser.dir/jvm_jar_reader.cpp.obj
src\jvm_parser\jvm_jar_reader.cpp(23): fatal error C1083:
    Cannot open include file: 'zlib.h': No such file or directory
```

Both parsers read ZIP containers -- a JAR and an APK are ZIP files -- so both
`#include <zlib.h>`, and both declare the dependency as the bare link name
`z`:

```cmake
target_link_libraries(retdec-jvm-parser PUBLIC retdec-bc-module z)
```

A bare `z` carries no include directory. On Linux it works by accident:
`zlib.h` is in the default include path and `libz.so` is in the default
library path, so neither half of the dependency has to be declared correctly.
On Windows neither is true, and nothing else in the tree supplies zlib for a
native MSVC build. `deps/zlib/` exists but returns immediately unless
`RETDEC_BUNDLED_ZLIB_WIN_CROSS`, which `cmake/options.cmake` defaults ON only
for `CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Windows"` -- the
MinGW cross path, not this one.

**The shape of the fix.** Build the bundled zlib for native Windows as well,
which cannot affect Linux or the cross path: `deps/CMakeLists.txt` adds the
subdirectory only under the cross condition, and `deps/llvm/CMakeLists.txt`
consumes `RETDEC_CROSS_ZLIB_*` only under the same one, so a native-Windows
branch is reachable from neither. Two details will cost an iteration each if
missed: zlib's CMake installs the static library as `libzlibstatic.a` under
MinGW but `zlibstatic.lib` under MSVC, and `zlibstaticd.lib` for a Debug
build, which `full-windows-debug` is. The two targets then need the include
directory and an `add_dependencies` on `retdec-zlib`, and the bare `z` should
stay only where a system zlib is what is meant.

**Not fixed here** because it is a build-system change for a platform this
environment cannot compile or test, and there may be more failures behind it
-- ninja stops at the first, and this is the first. `ctest-windows` is
`workflow_dispatch`-able and fails in about eleven minutes, so iterating on it
is cheap for someone who can watch it.

### The decompiler dumps core on C11 atomics

**Closed.** The symbolized stack named it exactly:

```
src/llvmir2hll/llvm/llvmir2bir_converter/llvm_instruction_converter.cpp:521:
visitInstruction: Fail (unsupported instruction:
  %var2 = atomicrmw add ptr inttoptr (i64 16412 to ptr), i32 1 seq_cst)
 #8  llvm_instruction_converter.cpp:521
 #9  InstVisitor<LLVMInstructionConverter, ...>::visitAtomicRMWInst
 #14 BasicBlockConverter::visitInstruction   basic_block_converter.cpp:287
```

`BasicBlockConverter` had no case for `atomicrmw`, `cmpxchg` or `fence`, so
all three fell through to the expression converter's `visitInstruction`,
which calls `FAIL` -- and `FAIL` aborts in release as well as debug. Eight
lines of C11 took the whole decompilation with them, and the guard directly
above the call site, which turns a null expression into an empty statement
rather than crashing, could never run because the abort came first.

All three have cases now (`basic_block_converter.cpp`). `atomicrmw` becomes
the read and the write it performs, `cmpxchg` becomes the read, the
comparison and a conditional store written as a ternary rather than as
control flow, and `fence` becomes nothing. The min/max and floating-point
`atomicrmw` forms are not lowered and take the empty-statement path, which is
what every other unhandled instruction in that converter does.

**What it cost to find it, and what that changed.** The first two DET-01 runs
printed thirty-six frames that named nothing: `tail -n 25` of a crash log is
the *bottom* of an LLVM backtrace -- `main`, `__libc_start_main` -- because
LLVM prints innermost frame first, and every frame read
`retdec-decompiler 0x00005643ba734e06` because the handler only runs
`llvm-symbolizer` when it can find one and otherwise falls back to `dladdr`,
which resolves exported symbols only. The excerpt is anchored on the report
banner now and prints the eighty lines above it, `llvm` is in the workflow's
apt list, and the third run named the instruction on the first line it
printed.
