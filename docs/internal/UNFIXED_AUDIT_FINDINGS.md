# Confirmed audit findings not fixed here

A 14-dimension subsystem audit of this fork produced 217 findings, 129 of them
high or critical. The ones that could be built and tested without LLVM were
fixed on the branch that produced this file; these could not, so they are
written down rather than attempted.

**Why they are not fixed.** A full build needs the LLVM 8-era pin from
`cmake/deps.cmake`, and the environment this audit ran in could not build it.
Changing `llvmir2hll` or `src/retdec/retdec.cpp` there would mean shipping an
edit to the C emitter that had never been compiled, let alone run against the
216-binary corpus. Everything below is therefore *read and verified against the
source*, but not tested.

Each entry names the file and line, states what is wrong, and says what the fix
is. Verify before acting: this tree moves, and a finding may already be fixed.

---

## 1. The 0/216 recompile failure has one cause, and it is three lines

`src/llvmir2hll/optimizer/optimizers/no_init_var_def_optimizer.cpp:28`

This is the finding worth acting on first. `README.md` reports default `.c`
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
`src/llvmir2hll/optimizer/optimizer_manager.cpp:263` describes one that does not
exist:

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

## 5. Two detectors that cannot fire on real input

Both are recall the fork is not getting and could.

`src/algo_recover/binary_search_detect.cpp:379` — the only genuinely structural,
def-use-based algorithm detector is unreachable in production, because the
pipeline's SSA producer (`src/retdec/llvm_to_ssa.cpp`) never sets
`IrInstr::defValue`. `binary_search` is one of only two `AlgorithmKind` labels
that survive harness extraction, and it can never be produced from a real
binary. Its unit tests pass because they hand-build IR with `defValue` set.

`src/algo_recover/find_detect.cpp:88` — `hasImmediateComparand` can never be true
on real input for the same reason, so `std::find` is always downgraded to
`std::find_if` and `std::count` to `count_if`.

Both are downstream of one defect: the LLVM-to-SSA adapter does not populate the
fields the detectors were designed against. Fixing that adapter is likely worth
more to the name-blind F1 than any individual detector change.

---

## 6. Other confirmed items

`src/fileformat/utils/format_detection.cpp:373` — uint32 overflow in the PE
lattice-hint bounds check, giving a ~4 GB heap over-read. Reached from
`detectFileFormat` on every non-raw input, and once per archive member.

`src/retdec/retdec.cpp:212` — two mutable process-wide statics are mutated
concurrently by `parallelBatchDecompile`: a `std::string` and a
`std::vector<std::string>` that N threads `push_back` and `clear` at once. Heap
corruption, not a garbled log line.

`src/neural/llama_inference.cpp:242` — `llama_tokenize` is called with
`parse_special=false`, so the ChatML template `prompts.cpp` builds is fed to the
model as literal text and never applied. Every non-mock refinement runs the
model outside its instruction-tuned distribution.

`src/retdec/semantic_recovery_export.cpp:95` — semantic comments are placed by
DWARF/PDB *source* line number, so on a stripped binary (the normal input) they
never appear, and where debug info exists they land at arbitrary offsets in the
emitted C.

`src/retdec/semantic_recovery_export.cpp:1583` — comment injection and sidecar
generation both run unguarded on the output file, including when `-f json` was
requested. The machine-readable mode emits invalid JSON whenever the input has
debug info and any detection fires.

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
