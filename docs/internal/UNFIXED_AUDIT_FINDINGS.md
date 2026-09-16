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

**The heading is wrong, and the number is measured now.** It took eight causes,
not one — this pass, the libc arity mismatch, four separate ways of losing a
goto's label, a `break` hoisted out of the loop it belonged to, a `void *` as
an operand of `|`, and `pthread_create` called with one argument where the
header declares four. The heading is left as written because the rest of the
section argues from it; the correction is here rather than in a rewrite.

**216/216.** CC-01 runs over the whole corpus on every `ctest-linux` run and
measured **216 of 216** at run 276, against the 0/216 this section was written
to explain. Both it and the 24-binary slice are floored at 1.0000. The trail is
runs 274 (208/216), 275 (214/216) and 276 (216/216), with F1 at 0.2302 and
DET-01 at 72 binaries 0 skipped throughout.

**Closed by 86970d0, by the narrow route.** The pass is no longer run:
`optimizer_manager.cpp` now carries a comment in place of the `run<>` call
naming the three facts in this tree that break the safety argument the old
comment made. The pass itself is untouched and still deletes every
initialiser-less `VarDefStmt` unconditionally, so it must not be re-enabled
without the use check described below; it is correct only for a back end that
emits no declarations, and this tree has only `c_hll_writer`.

**The 0/216 number was unmeasured, not fixed.** Nothing had ever handed the
emitted C to a compiler -- DET-01 compares two runs, CACHE-05 compares cache on
against cache off, the recovery gate reads detections, and all three treat the
output as text. `scripts/ci/check_emitted_c_compiles.sh` (CC-01) is that
missing measurement and runs in ctest-linux.

Its readings, in order: 21/24 on `d9e8108`, 23/24 on run 266 once the libc
arity mismatch was fixed, 24/24 on run 272 once the last of the goto-label
routes was. The floor is 1.0000 now, so a regression turns the run red rather
than being noticed a round later. Sections 2 and 3 below were the other
whole-file failures and are closed; a jump to 216/216 was never expected from
this section alone, and it did not come from it.

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

**~~`if_to_switch_optimizer.cpp` — range comparisons become single cases.~~**
**Fixed, and four tests asserted the defect.** An unbounded `v >= k` / `v > k`
else-if chain became a `switch` whose cases are single values:

```c
if (v >= 2) b = 2; else if (v >= 1) b = 1; else b = 0;
switch (v) { case 2: b = 2; case 1: b = 1; default: b = 0; }
```

`v = 7` sets `b = 2` in the first and `b = 0` in the second. Every value above
the highest bound is wrong, signed or unsigned. The four conversions are gone —
both `>=` and `>`, in the chain and the single-clause shape — and three tests
assert the chains stay as they are.

A correct construction exists for the *unsigned* case, and the comment where
the calls used to be says what it is: the top clause is the unbounded one, so
it belongs in the `default` and the else-clause body becomes a case. It is a
different transformation, it is wrong for signed compares for the reason below,
and it should arrive with F1, DET-01 and CC-01 behind it.

**~~`if_to_switch_optimizer.cpp` — signedness ignored.~~** **Fixed.** The
`Lt` and `Le` upper-bound patterns did not distinguish the signed compare from
the unsigned one, so `if (v < 1) A; else if (v < 2) B; else C;` on a signed
control expression became `switch (v) { case 0: A; case 1: B; default: C; }` and
every negative input took `C`.

The information was there: `ICMP_ULT` converts to `LtOpExpr::Variant::UCmp` and
`ICMP_SLT` to `SCmp`. The `Lt` matcher never read the variant; the `Le` matcher
read it only to require that every clause agreed with the first, which a chain
of signed compares does. Both require `UCmp` now.

`ElseIfLeUpperBoundChainConvertsToSwitch` built its chain with `SCmp` and
asserted the conversion — a test that encoded the defect. It is `UCmp` now, and
the signed shapes have tests of their own asserting refusal. Verified by
restoring the original file: all five new tests fail.

The nested and hoisting conversions in the same pass are unaffected and were
checked: their switch cases come from exact `v == k` tests rather than from
bounds, so a negative or out-of-range value matches no case and reaches the
same default it reached before.

**~~`copy_propagation_optimizer_ext.cpp` — propagation across an if.~~**
**Measured; does not reproduce, and the reading was right about the wrong
thing.** `canPropagateThroughStmts` is exactly as described: its walk is
`cur = cur->getSuccessor()`, so an IfStmt's body is stepped over, and
`isWrittenIn` only recognises an AssignStmt whose LHS is the variable — which
an IfStmt never is.

It is a *permission*, not the decision. `t = f(); if (c) { t = 0; } g(t);` gives
`g(t)` two reaching definitions of `t`, so `CopyPropagationOptimizer` takes its
"more than one definition" branch and refuses at the first check there, that all
the definitions are identical. Measured on a well-formed module: the argument
stays a variable. Forcing `canPropagateThroughStmts` to return `true`
unconditionally does not change it; bypassing the use-def branch does, which is
what makes the test evidence.

Two cases in `copy_propagation_optimizer_tests.cpp`: the dangerous shape is
refused, and an `if` that touches nothing the propagation cares about still
propagates — so a future repair of the walk cannot quietly become "refuse
everything".

One note for whoever reads the entry next: the first attempt at this
reproduction used a bare `Variable` named `f` rather than a declared `Function`,
so `getFuncByName` returned null and the optimizer bailed four guards earlier.
It looked like a refusal and was a malformed test.

**~~`while_true_to_for_loop_optimizer_ext.cpp` — loop direction by name.~~**
**Fixed, and three tests asserted the defect.** The function is
`isNonNegativeExt`, and its second branch was a substring match on the
variable's *name* — `size`, `len`, `cnt`, `num`, `idx`, `uint` — returning
`true` whatever the type.

A name is not a fact about a value. `int size = -1` is the commonest sentinel
there is, and a signed `idx` walking backwards is routine. What the answer
decides is whether a loop's exit test may be rewritten from `indVar != endValue`
to `indVar < endValue`, which is equivalent only when the step is non-negative:
with a negative step and `start > end`, `!=` runs the loop and `<` does not run
it at all.

The branch is gone. The type still answers when it can, which is sound and is
what should have decided it. The three tests — one per name, each on a *signed*
32-bit variable — now assert the refusal, and a fourth says the name stops
mattering in both directions. Verified by restoring the heuristic: all three
fail.

---

## 4. The public API cannot report failure — **all four fixed, and now read**

This section stood as an open list long after the code stopped matching it,
which is its own finding: a doc that sends the next reader to re-fix finished
work. Each item was checked against the source before this note was written.

* ~~`src/retdec/retdec.cpp` — `decompile()` is declared `bool` and returns
  `EXIT_SUCCESS`, which is `0`, which is `false`.~~ **Fixed.** It returns
  `true`, and the comment at that return records what the defect was.
* ~~`parallelBatchDecompile()` consequently reports every job as failed.~~
  **Fixed** by the above: `results[i] = futures[i].get()` takes `decompile()`'s
  answer directly, and a thrown exception still gives `false`.
* ~~`src/llvmir2hll/llvmir2hll.cpp` — failing to open the output file exits 0
  having written nothing.~~ **Fixed.** `getOutputStream()` returns `{}` on the
  error code and the caller returns `false`.
* ~~`src/retdec-decompiler/retdec-decompiler.cpp` — there is no unknown-option
  diagnostic anywhere in the CLI parser.~~ **Fixed.** `ProgramOptions::load()`
  throws `unknown option: <opt> (see --help)`, and `main` catches it and
  returns `EXIT_FAILURE`.

**None of the four was read by anything, which is how all four got in.** An
exit code nothing checks is an exit code nothing keeps right, and the code was
correct here only because somebody had recently looked.

`scripts/ci/check_cli_exit_codes.sh` (CLI-01) reads them, from ctest-linux where
the built decompiler is: an unknown option must exit non-zero, an unopenable
`-o` path must exit non-zero, and — the part that matters — a real decompilation
must exit zero having written something, because a gate that only asserts
failures passes on a decompiler that fails at everything.

Its self-test runs in the push gates and is two stub decompilers: one behaving
as the source does now, one exiting 0 for everything, which is the state this
section recorded. The check must pass the first and reject the second by name.

---

## 5. The SSA the detectors run on is not the SSA they were written against — **fixed and gated**

`src/retdec/llvm_to_ssa.cpp`

**Both halves are done, and this section stood open after they were.** That is
the second time in this file — section 4 was the first — and it costs what a
hidden defect costs: the next reader is sent to redo finished work. Checked
against the source before this note was written:

* `IrInstr::defValue` **is** assigned. `buildSsaModule`'s third pass sets it for
  every instruction with a non-void type, from the value map. The comment at
  `translateInstr` says why it cannot be done one instruction at a time: a phi
  at a loop header names a value from the latch, which has not been translated
  yet.
* **Every operand becomes a use**, for every translated instruction. The fourth
  pass is exactly the shape "The fix" below prescribed — a memo minting one id
  per LLVM value, a fresh `Immediate` per constant occurrence, a shared
  `VirtualReg` id otherwise — and the four-instruction-class restriction and the
  `ConstantInt`-only `continue` are both gone.

It is gated, too, which is the part section 4 was missing.
`scripts/ci/check_llvm_adapter.sh` (ADAPT-01) builds this file against the
system LLVM and runs `tests/retdec/llvm_to_ssa_test.cpp`, whose assertions are
not "some instructions define something": one requires that **every**
value-producing instruction defines a value, and another requires that the
def-use graph is actually connected — that at least one used value is one some
instruction defines. Either would fail on the state described below.

**What no longer follows.** The table further down lists four detectors as dead
on production input. That consequence rested on the adapter, and the adapter
delivers now. Whether each of the four contributes is a separate measurement —
the F1 gate reports the aggregate, not per detector — so the table is left as
the record of what was found rather than rewritten into a claim nothing here
took.

The rest of this section is kept as written, because the reasoning is what made
the fix possible and the "how it was found" is worth more than the list.

---

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

~~Eleven of the thirteen test sources in `tests/fileformat/` are named by no
`CMakeLists.txt`. Whether they still compile against the LLVM 23.1.0 migration
is not known here.~~ **Measured; they all compile, and they all run.** The
"not known here" was the load-bearing part of this entry and it was wrong:
`scripts/ci/check_fileformat_tests.sh` (FF-01) already compiled `src/fileformat`
against the distribution `llvm-dev`, so adding the directory was one line.
Every one of the sixteen files in `tests/fileformat/` compiles, and 83 of the
suite's 129 cases had been evaluated by nothing.

They stay out of the CMake target because nothing builds it — `ctest-linux`
names `retdec-decompiler`, `retdec-gui` and `retdec-gui-tests` and no other —
so listing them there would move them from one gate to none.
`scripts/check_cmake_sources.sh` carries them with that reason now, rather than
with "not yet re-enabled".

`tests/common/calling_convention_tests.cpp` is still unbuilt and still in that
list.

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

## Behind a build this environment cannot run — except it is not

This section's heading was the reason nothing here was attempted: "everything
below is compiled only by the LLVM-dependent build, so a change to it here would
ship untested — all were read and measured against the source; none were
compiled."

That was checked and it is false. `scripts/ci/check_fileformat_tests.sh` (FF-01)
compiles all of `src/fileformat`, `src/loader`, `src/common`, `src/pelib` and
`src/serdes` against the distribution `llvm-dev` and has done since it was
written — which is every file in the list below but the last, and that one is in
`src/cli_parser`, a module `standalone_check.sh` already builds. The premise held
when the audit was written and stopped holding when FF-01 landed; nobody went
back and re-read it. It is the fourth exclusion reason on this branch to survive
right up until somebody measured it, after `tests/cpdetect`+`tests/loader`,
`tests/unpacker`, and the `llvmir2bir_converter` exclusion on L2H-01.

So these were ordinary findings with an ordinary gate behind them. All seven
are resolved now, and the split is worth stating: **three were real** — the
`getRealSizeInRegion` clamp, the two loader clamps, and the CIL signature
traversal, which did not terminate on nine bytes — and **four were bounded by
something the entry had not looked for**: two readers that refuse an
out-of-range table, a check four calls upstream, and a short-circuit.

Each of the four says so below with the measurement, and three of them were
given a local bound anyway. Being unreachable is not the same as being
bounded, and the distance between the check and the arithmetic is what the
entries kept getting wrong in both directions.

* ~~`src/fileformat/file_format/elf/elf_format.cpp` and `coff_format.cpp` —
  `getDeclaredFileLength` overrides that shadow the base function which *was*
  fixed, each forming unclamped 64-bit sums (and, in the COFF case, an unchecked
  multiply) of header fields.~~ **Measured; not reachable, and the finding was
  wrong about why.** The sums are exactly as described — neither override
  clamps anything — but nothing gets hostile values into them.

  `ElfFormat` forms `max(shoff + sectionTableSize, phoff + segmentTableSize)`.
  ELFIO reports a table size of zero unless the table lies inside the file, so
  `e_shoff = 0xFFFFFFFFFFFFFFFF` arrives with a size of 0 and the sum is the
  offset itself. Driven at the top of the address space, with `e_shnum` at
  `0xFFFF`, the declared length comes back as `18446744073709551615` — the
  offset, unwrapped.

  `CoffFormat` forms `symbolTableOffset + numberOfSymbols * entrySize` and then
  `declaredSize + sizeOfStringTable`. LLVM's `COFFObjectFile` reports zero
  symbols for a table it could not read, which is the condition the override's
  own `if` already tests, and `getSizeOfStringTable` refuses an offset past the
  end before reading. With both 32-bit fields at `0xFFFFFFFF` the symbol count
  is 0 and the string-table size is 0. The multiply is 32×32 into a 64-bit
  `std::size_t` in any case, so it cannot overflow on any host this tree builds
  for; on a 32-bit one it would, and nothing here asserts otherwise.

  That makes both sums safe because of code some distance from them, which is
  not a thing to leave unwritten. `tests/fileformat/declared_file_length_tests.cpp`
  pins the invariant in five cases: a reader that starts reporting an extent for
  a table it could not read makes the sum wrap and the declared length fall
  below the offset it was formed from. Verified by making
  `ElfFormat::getSectionTableSize` return a size for a table ELFIO declined —
  the test fails, naming that.
* ~~`src/fileformat/file_format/file_format.cpp` — `getXByte` still forms
  `secSeg->getOffset() + secOffset`.~~ **Fixed, though not reachable.** The sum
  is real: `sh_offset` is unbounded — ELFIO validates that the section *table*
  lies in the file, not that each section's data offset does — so at
  `0xFFFFFFFFFFFFFFFF` it wraps to a small number the containment test below
  finds comfortably inside the file, and the read returns the wrong bytes from
  a valid address. It is not reachable because a section whose `sh_offset` is
  outside the file has nothing loaded, so `getLoadedSize()` is 0 and the *first*
  `rangeFitsWide` refuses before the second is evaluated.

  That is a property of the loader, and the sum is here. `regionEndSaturating`
  is the same arithmetic without the wrap, is proved over the whole 64-bit
  domain, and has cases in `tests/loader_sim/xbyte_width_guard_test.cpp`. Every
  offset that did not wrap is unchanged. No new test: the kernel is tested and
  the substitution cannot change a non-wrapping answer — pinning the call site
  would need an ELF with a valid section table and a hostile `sh_offset`, which
  is worth building the day something makes this reachable.

* ~~`file_format.cpp` (`computeSectionTableHashes`) and (`getOverlayEntropy`) —
  unchecked multiply and sums on header fields.~~ **Measured; neither can
  wrap.**

  `getOverlayEntropy` computes `bytes.size() < declSize + overlaySize`, and the
  sum cannot wrap by construction: `getOverlaySize()` returns `realSize -
  declSize` when `realSize > declSize` and 0 otherwise, so a non-zero
  `overlaySize` makes `declSize + overlaySize` exactly `realSize`, and a zero
  one short-circuits the `||` before the sum is formed. The read after it is in
  bounds for the same reason.

  `computeSectionTableHashes` forms `getSectionTableOffset() + i *
  getSectionTableEntrySize()` with `i` bounded by `sections.size()`. A non-empty
  `sections` means ELFIO loaded the table, which means `e_shoff` is inside the
  file — the same invariant `tests/fileformat/declared_file_length_tests.cpp`
  already pins, since it asserts a table size of zero for an out-of-range
  offset and that size is `sections.size() * entrySize`. `i * entrySize` is at
  most `sections.size() * 65535`, both bounded by the file, so the product does
  not overflow a 64-bit `std::size_t` either.
* ~~`src/fileformat/utils/other.cpp:407` — `getRealSizeInRegion`'s wrapping
  clamp.~~ **Fixed.** `offset + requestedSize > regionSize` is now
  `requestedSize > regionSize - offset`, which asks the same question without
  the sum; `offset < regionSize` is established two lines above, so the
  subtraction cannot wrap either. Six cases in `tests/fileformat/other_tests.cpp`,
  four of them pinning the in-range answers so the clamp cannot be tightened by
  accident. Verified by restoring the old line: the two wrapping cases fail and
  the four in-range cases do not.
* ~~`src/fileformat/types/resource_table/bitmap_image.cpp` — unchecked
  `nBytesInRow * nRows` from header width/height/bitCount, feeding the clamp
  above.~~ **Measured; not reachable, and the number is worse than the entry
  said.** The product is right and it is not a wrap: at width `0xFFFFFFFF`,
  height `0xFFFFFFFF` and bitCount `0xFFFF` it is `1152903920473874428` — 1.15
  exabytes — which is *below* `std::vector<std::uint8_t>::max_size()`, so
  `bytes.reserve` of it is an allocation attempt rather than a
  `std::length_error`. It raises `std::bad_alloc`, and nothing between
  `BitmapImage` and `main()` catches one. `image.reserve(height / 2)` is 51 GB
  of `std::vector` on its own.

  Nothing gets there. `parseDibFormat` is the only caller of any
  `parseDib*Data`, and it runs `parseDibHeader` first, which refuses
  `width > 512`, `height > 1024` and `bitCount > 32` — so `nBytes` on that path
  is at most `2048 * 512`, one megabyte.

  The bound is four calls from the multiplication, nothing near it says so, and
  every `parseDib*Data` is public. So the reserves are now clamped to the icon's
  own loaded size with `bounds::reserveFor`, which is a cap on the *hint* and
  not on the parse: `reserve` only pre-allocates, the containers still grow to
  whatever is read, and the `bytes.size() != nBytes` test after each read
  already refused anything short. No input that parsed before parses
  differently.

  `bounds::reserveFor` is ESBMC-proved and, until this, had no callers anywhere
  in the tree. It was written for this shape.

  `tests/fileformat/bitmap_image_tests.cpp` calls the public methods directly
  with a header `parseDibHeader` would have refused. Two of the six are
  evidence: 24 and 32 bpp have no palette to read first, so they reach the
  reserves, and removing the clamp makes exactly those two fail with
  `std::bad_alloc`. The other four say so in the file rather than being counted.
* ~~`src/loader/loader/segment.cpp`, `segment_data_source.cpp` — post-hoc and
  pre-copy wrapping clamps. `Image::getXBytes` now refuses before them, but both
  have other callers.~~ **Fixed.** These were the real ones in this list. Unlike
  the `getDeclaredFileLength` overrides and the `BitmapImage` product, nothing
  upstream bounds them and what they feed is a `std::copy`:

  ```cpp
  loadSize = loadOffset + loadSize >= getDataSize() ? getDataSize() - loadOffset : loadSize;
  std::copy(_data.data() + loadOffset, _data.data() + loadOffset + loadSize, ...);
  ```

  At `loadOffset` 1 with `loadSize` `0xFFFFFFFFFFFFFFFF` the sum is 0, which is
  not `>=` any data size, so the clamp is skipped and the copy reads
  18446744073709551615 bytes out of a four-byte buffer. `saveData` is the same
  shape with the copy going the other way, and `Segment::getBytes` is the same
  shape ending in `result.resize(size, 0)`.

  All three now compare against the space that remains, which the guard two
  lines above each already established cannot wrap — the same repair as
  `getRealSizeInRegion`, and equally answer-preserving: where the old test
  clamped on equality, so does the new one.

  `saveData` also gained a bound it never had: `saveSize` was checked against
  the destination and never against the source, so `saveData(0, 100, aVectorOfTen)`
  read ninety bytes past the end of the caller's vector. Every caller in this
  tree passes a long enough vector; none of them says so.

  Five cases in `tests/loader/segment_data_source_tests.cpp` and three in
  `segment_tests.cpp`. Verified by restoring each old line: four fail for
  `SegmentDataSource` and one for `Segment`. The two that do not are labelled in
  place — one is the guard the clamps rest on, and the other uses offset 0,
  where the sum does not wrap and the old clamp was right.
* ~~`src/cli_parser/cli_sig.cpp` — a self-referential TypeSpec whose signature
  is a GENERICINST with two or more self-referencing type arguments drives an
  exponentially wide traversal under the depth-64 bound.~~ **Fixed, and it
  reproduced.** This entry was right, including the byte sequence. As a field
  signature it is nine bytes:

  ```
  06 15 12 06 02 12 06 12 06
  ```

  FIELD, GENERICINST, CLASS TypeSpec row 1 — the row itself — two arguments,
  each the row itself again. Two branches per level, 64 levels: 2^65 nodes.
  `kMaxTypeDepth` never trips, because the cycle never gets deeper than 64.

  Measured against a resolver modelled on `CLIReader::typeSpecType`: **2,000,000
  resolver calls in 0.69 s, and still descending** when a counter in the test
  stopped it. At that rate 2^65 nodes is about 10^4 years. It does not
  terminate.

  The fix is not the visited-set this entry proposed — that needs
  `ITypeNameResolver` to change, and its other implementation is behind the LLVM
  build. A budget does the same job from inside this file: `kMaxTypeWork`
  counts levels *entered* rather than levels *deep*, in a `thread_local` beside
  `g_typeDepth`, reset only when a descent begins at depth zero. The cycle
  re-enters through the resolver with the enclosing frames still holding their
  guards, so it never sees zero and never gets a fresh budget. 100000 nodes is
  about 33 ms at the rate above, and four orders of magnitude above what a
  language compiler emits — `Dictionary<string, List<int[]>>` is a dozen nodes.

  After: exactly 100000 calls, in 40 ms.

  Three cases in `tests/cli_parser/cli_sig_width_test.cpp`. The resolver keeps a
  cap of its own so that a regression fails the test in about two seconds
  instead of never returning — a test that hangs is not one anyone can run.
  Verified by removing the budget: the branching case fails and names the line.
  The other two are labelled controls — the non-branching cycle, which the depth
  bound already handled, and an ordinary nested generic, which a budget that
  refused real work would break.

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

~~What is still open is one step further: the CUDA half is **compiled but not
executed** by the suite.~~ **Closed.** It is executed now, by the second test
binary this entry proposed.

Two things were needed, and neither was the `GpuScanner` split the entry
offered as the alternative:

* **The object.** `gpu_scanner.cu` is compiled without
  `RETDEC_GPU_SCANNER_HOST_ONLY` and passed to the link *explicitly*, so the
  linker satisfies `GpuScanner`'s methods from it and never pulls
  `gpu_scanner_cpu.o` out of `libutils.a`. Archive members are only taken to
  resolve what is still undefined, so the two never collide.
* **A device.** `cudaMalloc`, `cudaMemcpy` and `cudaFree` were already real host
  operations, but `cudaGetDeviceCount` returned 0, so `ensureGpu()` took its
  "CUDA absent" path and every method fell back to the host code the `utils`
  suite already covers — the kernels would still not have run. The stub
  presents one device now, behind `cudastub::presentOneDevice()`, off by
  default so every existing user of the header sees what it saw before.

**The assertions are `tests/utils/gpu_scanner_tests.cpp`, unchanged and not
copied** — the same 17 the `utils` suite runs against the CPU path. Two
implementations of one class, one set of assertions, and they have to agree.
That is worth more than a second set written for the device path alone, which
would only ever say what its author expected.

Verified by repeating the experiment this section describes. An off-by-one in
`findAllKernel` — `pos + needleLen >= dataSize` for `>` — is inside the
device-only region:

```
  ok  utils — 351 tests ran
 FAIL cuda half (tests)
[  FAILED  ] GpuScannerTests.UploadedFileIsScannable
```

The CPU suite does not notice, which is the state that let four fixes be
reverted with the suite staying green at 13/13. The new check does.

The first version of it reported `runs 0 test(s)` and returned 0, because the
count came from a `[ OK ]` grep that `--gtest_brief` suppresses. A binary that
runs no tests passes every assertion it has, so the count is read from the
"N tests ran" line now and floored at `CUDA_HALF_MIN_TESTS`.

What the stub still does not model is at the top of
`tests/utils/cuda_stub/cuda_runtime.h`: no warp semantics, no coalescing,
blocks serialised rather than concurrent. A kernel with an inter-block race
passes here and fails on a device.

### The Imortek C back end is compiled, unit-tested, and linked into no tool

`CodeGenPass::generateFunction()` and `generateUnit()` — the entry points of
`src/codegen`, which turns SSA plus a `StructNode` tree into a C AST — have
no caller anywhere in `src/`. Their only callers are `tests/codegen`.

The link graph says the same thing from the other end. `retdec-codegen` is
linked by exactly one target, `retdec-cxx-backend`; `retdec-cxx-backend` is
linked by exactly one target, `retdec-cxx-backend-tests`.
`retdec-decompiler` links `retdec::llvmir2hll` — the Avast back end — and
neither of the two. So `src/cxx_backend` consumes a `codegen::CUnit` that
nothing outside a test ever produces.

This is not the same as `src/opencl`, which no `add_subdirectory` names:
these are built, warned about, and unit-tested on every run of
`scripts/standalone_check.sh`. What they are not is *reached*. Every defect
found in them on this branch — the goto with no label, `CondNormaliser`
having no comparison to match, `GotoEliminator` having no label to fold
against, `Stats` counters that could only read zero — is a defect in code
the shipped decompiler does not execute.

That does not make the fixes wrong; a back end being built toward should be
correct before it is switched on, and each was verified by reverting it. It
does mean **none of them can explain a CC-01 failure**, which measures
`retdec-decompiler` and therefore `llvmir2hll`. Where a commit message on
this branch says the emitted C was broken by one of these, read it as the
code being broken rather than the output — the correction below says so
where it matters.

Closing this is not a small change and is not attempted here: it means
deciding whether the Imortek back end replaces `llvmir2hll` or sits beside
it behind an option, and either answer is a design decision with a corpus
measurement attached, not a wiring fix.

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

### The CMake half is still open; "compiled by nothing" is not

**Partly closed by OCL-01.** The reason above is about `find_package`, and the
sources do not need CMake to be compiled: they include `<CL/cl.h>`, the
standard library and their own headers, and no LLVM. Checking that was one
command, and the entry above stood for as long as nobody ran it.

`scripts/ci/check_opencl_tests.sh` compiles all 19 translation units, builds
every kernel, and runs all eight suites. It is in CI and in the push gates.
What that found, none of which any check could have seen before:

  * **`src/opencl` did not compile.** `ocl_type_inferencer.cpp:297` does
    `ptr[sl] |= (opPtr[i] != 0)` on a `std::vector<bool>`, whose `reference`
    proxy has no `operator|=`. Four lines up, `cpu_dsu::unite` spells the same
    merge as `ptr[ra] || ptr[rb]`.

  * **Three of the five kernels did not build**, and so did three of the five
    *embedded copies* — `schar` is not an OpenCL C type, `&(uint)expr` is not
    an lvalue, and `atomic_inc`'s cast to `atomic_uint*` made the call
    ambiguous. There are two copies of every kernel: `kernels/*.cl`, and a C++
    raw-string literal in `src/opencl/*_sources.cpp`. **The runtime compiles
    the embedded copy**, so a check that read only the `.cl` files would have
    checked the copy that never runs. They are not the same text — the
    embedded `semantic_hash` is 832 lines against the `.cl` file's 952 — and
    the comment claiming they are "embedded verbatim" is wrong.

  * **The BITFIELD rewrite could only ever fire for a shift of zero.** Rule 5
    turns `(x >> k) & mask` into a bitfield extract, and checked contiguity of
    `mask >> k`. The mask is applied *after* the shift, so it already sits at
    bit 0; shifting it again by `k` zeroed every mask narrower than the shift.
    Present identically in the CPU path and both kernel copies.

  * **The CPU emulator decoded almost nothing.** `host_emu::emulate` handled
    NOP, RET, jumps, push/pop and `MOV reg, imm`. Every other opcode advanced
    the instruction pointer by one byte and carried on — which resumes
    decoding inside the next instruction, so every byte after the first
    unknown one is read as an opcode. `xor eax,eax` and `mov rax,42` were both
    unhandled, which is why two functions with different behaviour hashed
    identically. It has a ModRM core now (the ALU groups in both directions,
    `TEST`, the `MOV` forms, `MOVZX`/`MOVSX`), and an opcode it does not model
    stops the run and reports `Unsupported` instead of producing a hash from a
    desynchronised stream.

**Why none of this showed up as a failure.** `OCLContext` falls back to the CPU
path when a kernel fails to build, and the GPU tests assert only that they get
the right number of non-zero hashes. So they passed whether the kernel compiled
or not — and they did. That is why OCL-01 compiles the kernels itself rather
than trusting the suite, and why it requires zero skips when a device is
present.

**What is still open.** The CPU fallback and the kernel are two implementations
of one emulator, and they are not at parity: the kernel has a real
instruction-length decoder, memory operands, `CALL` with a depth limit, string
operations and the flag instructions; the CPU has none of those. No test
compares the two against each other, so a divergence in the shapes both handle
would not be caught. The CMake wiring above is also still open — OCL-01 builds
these sources, `cmake` still does not.












## The README published a number its own CI contradicted

`README.md`'s results table led with

| Recompile, default `.c` | 0/216 | 0/216 |

and explained it: `NoInitVarDefOptimizer` deletes every initializer-less local
declaration, so the emitted C assigns to variables it never declares —
"unfixed because it cannot be tested without the LLVM build".

Every clause of that was out of date:

* The pass is **not run** for the C back end. `optimizer_manager.cpp` says so
  and gives three reasons, each with a file and line.
* It **can** be tested without the LLVM build.
  `scripts/ci/check_llvmir2hll_tests.sh` runs that component against the
  system LLVM in about four minutes.
* The number is **contradicted by this repository's own CI**. CC-01 hands the
  emitted `.c` to a compiler on every `ctest-linux` run and measured 21/24,
  then 23/24, then 24/24 — against a published 0.

A stale measurement is replaced by another measurement, not by an argument,
so CC-01 now runs over the whole 216-binary corpus on **every `ctest-linux`
run**, not weekly. That cadence was a measurement too: run 272's CC-01 step
took ten seconds for 24 binaries, so 216 is about ninety seconds against a
job that already spends eight minutes building and seventeen in clang-tidy.
It went into the nightly first, on the assumption that a full-corpus pass was
expensive; the step timings said otherwise and it moved.

No `--min-rate` on the full corpus yet: it reports, and the number becomes
the floor in a follow-up commit that names the run it came from, exactly as
the 24-binary slice was introduced. Until that number exists the tables say
so, rather than printing a 0 nobody believes or a 216/216 nobody has
measured.

Stock RetDec's `0/216` is untouched — it was not re-measured and no claim
about it changes.

## `lab_0x112c`, found

`splitWhileTrueLoop()` refuses to split a `while (true)` loop when something
jumps into it:

```cpp
while (currStmt) {
    // When a statement in the loop is a goto target, we cannot split the
    // loop. Otherwise, after optimizing the loop, we might end up with
    // incorrect code.
    if (currStmt->isGotoTarget()) {
        return {};
    }
```

The comment is right. The test is one level too shallow: `currStmt` walks the
body's own successor chain, and nothing nested inside it.

All three `while_true_to_*` optimizers call this function, and all three keep
`splittedLoop->beforeLoopEndStmts` as the new body and throw the loop-end
`if` away. The `break` or `return` inside that `if` is exactly where a goto
lands, and exactly what is nested rather than top-level. Its label went with
the discarded `if` while the goto naming it stayed —
`goto lab_0x112c;` with no `lab_0x112c:`, which is what CC-01 had reported on
`generated_shell_sort-gcc-O2` for five rounds.

The question is asked over the whole body now, once, with
`GotoTargetAnalysis::hasGotoTargets()`. Three lines.

**Run 272 measured the effect: CC-01 24/24, rate 1.0000.** Every emitted C
file on the slice compiles, and the floor is that number now. F1 stayed at
0.2302 and DET-01 at 72 binaries with 0 skipped, so declining to convert
loops something jumps into cost neither detection quality nor determinism.

**It refuses more than it strictly has to**, and deliberately. Only the
loop-end `if` and `afterLoopEndStmts` are discarded; a goto into
`beforeLoopEndStmts`, which the new body keeps, is harmless. Checking only
the discarded region would convert more loops than the old code did — the
old test covered the whole top-level chain too — so it is a behaviour change
beyond fixing the defect, and it needs the F1 and DET-01 numbers behind it
rather than an argument. The narrow version also has to run *after* the
split, since the split is what decides which parts are discarded. Left for a
round that can measure it.

### How it was found, after four wrong answers

Four earlier rounds each read the passes, found a real defect, fixed it, and
watched the error come back at the same line. What broke the pattern was not
a better guess:

1. **`--save-failures`** made CC-01 keep the emitted `.c` and the `.ll` the
   back end was given, instead of deleting both with its temp directory.
2. **The `llvmir2bir_converter` exclusion** came off `check_llvmir2hll_tests.sh`
   once the reason was measured rather than assumed, which is what let the
   converter run here at all.
3. **`OptimizerManagerPipelineTests`** turned that into a local pipeline:
   LLVM IR in, thirty passes, emitted C out, scanned for a label jumped to
   and never defined.
4. **One CI round** produced the real IR. Reduced to the failing function it
   is 200 lines, and it reproduced on the first run.
5. **Delta-debugging** `OptimizerManager`'s `enabledOpts` found the smallest
   set of passes that still stranded the label: one. Then the next one. Then
   the shared helper both of them call.

Five rounds of reading to narrow it to a subsystem; about two minutes to name
the function once the input could be replayed. The reduced IR is now a test,
so the next regression is caught before CI rather than five rounds after it.

One honest note: of the three per-pass regression tests,
`WhileTrueToForLoopAloneKeepsTheLabel` passes with the fix reverted —
`WhileTrueToForLoopOptimizer` declines this particular input earlier, for an
unrelated reason. It is kept as a control on the third caller, not claimed as
evidence. A unit test written to make it load-bearing was dropped rather than
kept: it also declined for the unrelated reason, so it could not fail either.

## `removePredecessors(onlyNonGoto)` removed gotos

```cpp
void Statement::removePredecessors(bool onlyNonGoto) {
    ...
    // We remove only non-goto statements.
    for (const auto &pred : preds) {
        if (pred->getSuccessor() == thisStmt) {
            toRemoveStmts.insert(pred);
        }
    }
```

The comment says non-goto and the test says fall-through, and a goto passes
the fall-through test whenever it sits immediately before the statement it
jumps to: in `goto L; L: ...` the goto's *successor* is its own target.

So "only non-goto" dropped that goto from `L`'s predecessors.
`Statement::isGotoTarget()` walks `preds`, so it then answered `false`;
`CHLLWriter::emitGotoLabelIfNeeded()` asks exactly that question and stopped
writing `L:` — while the goto naming it stayed. Both statements still
emitted, one label missing, which is the shape of `goto lab_0x112c;` with no
`lab_0x112c:`.

Five callers, and every one wants the same thing: `setSuccessor()`,
`prependStatement()`, `IfStmt::addClause()` and the two clause setters each
mean "this is now the one statement that falls through into you". A goto
does not fall through into anything — its successor is where it sits, not
where it goes. Gotos are skipped now; `removePredecessors(false)` still
means all of them, and has a test saying so.

This one is in `llvmir2hll`, which is the path `retdec-decompiler` runs.

### The emitted C is now checked for undefined labels without CI

`OptimizerManagerPipelineTests` gained the question CC-01 actually asks. It
converts LLVM IR, runs the pipeline, hands the module to `CHLLWriter`, and
scans the emitted C for a label jumped to and never defined — resetting at
each function, because a C label is function-scoped. `label 'lab_0x112c'
used but not defined` is a compiler asking this; asking it here costs two
minutes instead of a corpus binary, a front end and a thirty-minute round
trip. The scan has its own test, including a label defined in the *next*
function, since an assertion that cannot fail is worse than none.

Getting the writer to run at all needed one fix.
`HLLWriter::emitAddressRangeForFuncIfAvailable()` decided "available" by
comparing against `NO_ADDRESS_RANGE`, which is `AddressRange(0, 0)` — a
sentinel. A range that is merely *undefined*, which is what a `Config`
returns when it knows nothing about the function, is not equal to that
sentinel, so it went past the guard into
`Address::toHexPrefixString()`, which asserts `isDefined()`. The guard asks
whether the endpoints are defined as well now. The shipped `JsonConfig`
returns the sentinel, so this was latent there; it aborts immediately on a
config that does not.

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

**And the goto eliminator had never eliminated anything.**
`GotoEliminator` matches a goto against a `Kind::Label` statement —
`countLabelsInTree()` collects them and `rewriteStmtTree()` closes its guard
at one. With no label ever created, that set was always empty, no goto was
ever eliminable, and `Stats::gotosEliminated` was structurally zero on every
input: the same shape as `condRewrites` and `castsRemoved` one section down,
a pass that cannot fire beside a counter that cannot move.

Emitting the label turns the pass on. It is now the reason the first test
above has to set `enableGotoElim = false` — with it on, the eliminator folds
that goto away, which is the pass working and would have hidden the
emitter's half. A third test asserts the other side: with elimination on, the
same input reports `gotosEliminated == 1`, `gotosRemaining == 0`, and emits
no `goto` at all.

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

### "This was the last entry on the audit's list" — it was not

48145da's message says the `cli_sig.cpp` traversal "was the last entry on the
audit's 'behind a build this environment cannot run' list", and counts the list
as six with two real. It was seven, two of them still open when that was
written: `getXByte`'s offset sum and the `computeSectionTableHashes` /
`getOverlayEntropy` pair. Both are resolved in the commit that adds this note,
and the count that holds is: seven entries, three real — the
`getRealSizeInRegion` clamp, the loader clamps, and the CIL traversal — and
four bounded by something the entry had not looked for.

The mistake is the one this section keeps catching: a claim about a *set*,
written from the entry in front of me rather than from the list.


### The codegen goto fixes are not on the shipped path

`d4d9c9e` and `782117d` fix real defects in `src/codegen` — every `goto` it
emitted named a label nothing wrote, and `GotoEliminator` consequently had
nothing to eliminate against. Both messages describe the consequence as an
uncompilable translation unit, which is what the code would produce.

It produces nothing today. `CodeGenPass` has no caller outside
`tests/codegen`, and `retdec-decompiler` links `llvmir2hll` instead — see
the structural entry above. So neither fix changes any output the corpus
measures, and neither is a candidate explanation for CC-01's
`label 'lab_0x112c' used but not defined`, which comes from the `llvmir2hll`
path.

### `getType()` is not a portable spelling of `getPointerType()`

`ce86f3d` replaced `cNullPtr->getPointerType()` with `cNullPtr->getType()` and
said `ConstantPointerNull` "specialises `getType()` to return a
`PointerType*` in every LLVM this code has ever been built against". That was
checked against the system LLVM and asserted of the pinned one, which is the
same mistake as the exclusion the commit was undoing.

Run 270 failed to build:

```
llvm_constant_converter.cpp:236: error: cannot convert
'shared_ptr<Type>' to 'shared_ptr<PointerType>'
```

`typeConverter->convert()` is overloaded on `Type*` and `PointerType*`. The
system LLVM does specialise `getType()` and picked the `PointerType*`
overload; the pinned one does not, picked the `Type*` overload, and
`ConstNullPointer::create()` would not take the result. The call compiled
cleanly here and broke there.

`llvm::cast<llvm::PointerType>(cNullPtr->getType())` names the type at the
call site, so the overload chosen is the same whichever LLVM is in hand. That
is sound by construction rather than by which header happens to be on the
include path — which is what the original line was not, in either direction.

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

### `DerefOpExpr::getType()` returns the pointer's type, not the pointee's

`src/llvmir2hll/ir/unary_op_expr.cpp:27`

```cpp
ShPtr<Type> UnaryOpExpr::getType() const {
	return op->getType();
}
```

`DerefOpExpr` does not override it. So `*p` reports the type of `p`: ask
whether the operands of `*p | 8` are pointers and the answer is yes for every
well-typed expression of that shape in the tree.

Found by writing a scan for the `invalid operands to binary |` defect below and
watching it report a function whose emitted C was correct. The scan unwraps one
dereference itself; the IR was left alone.

Changing it is not a small change. Every consumer of a dereference's type in
the back end — the C writer's cast decisions, `ExprTypesAnalysis`, the
optimizers that compare operand types before rewriting — currently reads the
pointer type and has been tuned against that. A one-line fix here would move
all of them at once with no measurement behind it, and this branch has a rule
about that. It is written down so the next person to be surprised by a type
query finds this instead of re-deriving it.

The bar for doing it: a run with `DerefOpExpr::getType()` returning the
contained type, F1 and DET-01 unchanged, and CC-01 no worse than the floor it
had.

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

## A `#include` for a header that no longer exists

**Closed.** `GCCGeneralSemantics` assigned `<stropts.h>` to `ioctl` and seven
others, and `<libio.h>` to three. Assigning a header is not a note: it is what
makes `CHLLWriter` emit an `#include` for it --
`HeadersForDeclaredFuncs::getHeaders` collects one per declared function and
`c_hll_writer.cpp:376` writes them out. Neither header exists on a current
glibc. `libio.h` became internal in 2.28 and `stropts.h` went with STREAMS in
2.30, so any binary that declared one of those eleven produced C that failed at
its own first line.

`ioctl` is the one that matters: it is ordinary, and it is declared in
`<sys/ioctl.h>`, which is where it is assigned now. Of the libio three,
`__overflow` is still declared by `<stdio.h>` and moves there; `__uflow` and
`__underflow` are declared nowhere includable, so they get no header at all and
are emitted as declarations instead, which compiles. The other seven STREAMS
names get none for the same reason.

Moving `ioctl` also gave it an arity, which it could not have had before: the
arity table is measured against whatever header the semantics assigns, and
nothing is measurable from a header that cannot be included. It is
`(2, variadic)` now, matching `int ioctl(int, unsigned long, ...)`, so an
under-supplied `ioctl` call gets the cast the writer exists to add.

**How it was found, and why not earlier.** Not by reading the header map --
by ARITY-01's new header-availability probe, added for an unrelated
portability defect, which reported `<libio.h>` and `<stropts.h>` as assigned
but unavailable. CC-01 compiles all 216 corpus outputs and has been at 216/216;
it never saw this because no corpus binary calls any of the eleven. That is the
limit of a corpus gate stated plainly: it can only find defects the corpus
provokes, and `ioctl` is common in the world and absent from these 216.

**Gated twice.** Four tests in
`tests/llvmir2hll/semantics/semantics/gcc_general_semantics_tests.cpp` assert
the assignments directly; reverting the header map fails three of them. The
fourth reads the arity table, so its guard is ARITY-01 instead: with the header
map reverted the probe fails, naming both headers.

### The case that would still have been invisible, and is not any more

Fixing `ioctl` left the more uncomfortable question standing. It was caught
because `<stropts.h>` does not exist; a name assigned a header that *does*
exist but does not declare it fails in exactly the same way and leaves no
trace. ARITY-01 considers only two causes when a name will not compile under
its header -- a function-like macro, or not on this platform -- and records
either as "left out". A wrong-but-real header is silently the third, and the
emitted C gets an `#include` that does not declare the function it is for.

So it was measured. `--audit-assignments` takes every name the measurement
drops, excludes the ones that really are function-like macros under their own
header, and tries the rest under the union of every header the semantics
assigns. Anything that then compiles is misassigned.

**1968 names across both modules, 0 misassigned.** A clean result, which is
only worth the words if the check could have said otherwise: moving `strlen`
out of `<string.h>` makes it report `strlen`. It is not part of `--check` --
it is a few thousand extra compiles and the header map changes rarely -- so it
is a flag to run when that file changes.

That bounds the concern rather than closing it by assertion: the 555 names both
tables leave out are left out because they are macros or are genuinely not on
this platform, and not because the map sent them to the wrong header.


## Four architectures nothing had ever asked a question about

**Closed by C2L-01.** Asked to bring every CPU architecture to parity with
x86-64, the first thing to establish is what the gap actually is. It is not
what it looks like from the instruction tables:

| arch    | translated | listed | tests |
|---------|-----------:|-------:|------:|
| x86     |        331 |   1343 |  1965 |
| arm     |        175 |    435 |   534 |
| arm64   |        189 |    461 |   457 |
| mips    |        142 |    627 |   598 |
| powerpc |        283 |   1192 |   808 |

("listed" counts the entries in each translator's `_i2fm`; the difference is
entries mapped to `nullptr`, which fall through to a pseudo-call. x86 is at
24.6% by that measure, the *lowest* of the five, because it lists every SSE and
AVX form. A ratio over an ISA's own instruction count does not compare across
ISAs.)

The gap is not coverage. It is that **every end-to-end gate in this repository
measures exactly one architecture**: all 216 binaries in
`tests/algorithm_recovery/corpus` are x86-64, so CC-01, DET-01 and the F1
recovery gate — the three numbers this branch publishes — say nothing at all
about ARM, ARM64, MIPS or PowerPC.

And the one place their semantics *are* checked, `tests/capstone2llvmir`, was
on `UNGATED_SUITES`: 46,066 lines and 4,370 assertions evaluated nowhere. The
recorded reason was

> same LLVM 20+ drift (4-arg APInt, Intrinsic::getOrInsertDeclaration), and its
> tests need `<keystone/keystone.h>`, which deps/keystone is a download stub for

Both halves are facts about the machine rather than about the code.
`llvm-20-dev` is in Ubuntu noble's own universe pocket, and Capstone 5.0.9 and
Keystone 0.9.2 — the exact revisions `cmake/deps.cmake` pins — build from
source in about three minutes between them. With those present all 17
translator sources and all six test files compile **unchanged**, and all 4,370
tests pass. The suite was never broken. Nothing had run it.

The system Capstone is not enough, and that is where the "drift" reading came
from: it is 4.0.2, and the x86 translator names `X86_REG_BND0..3`,
`X86_INS_FUCOMPI` and `X86_INS_FCOMPI`, which arrived in 5.x. Six errors, and
reading them as drift rather than as the wrong Capstone is what kept the entry
standing.

**The floors are per architecture** — `MIN_X86`, `MIN_ARM`, `MIN_ARM64`,
`MIN_MIPS`, `MIN_POWERPC` — because a single total lets 1,965 x86 cases hide
the disappearance of all 598 MIPS ones while the aggregate barely moves.
Falsified by deleting `mips_tests.cpp`: `FAIL Mips: 0 test(s), floor is 598`,
with the other four still reported.

`tests/llvmir-emul` rides along, off the same list for the same reason; its
library is already linked because the translator tests execute the IR they
produce. 4,380 tests.

**What this does not close.** Parity of *coverage* is untouched: MIPS still
translates 142 instructions and PowerPC 283, and whether those are the ones
real binaries use is not measured here. Nor is there a non-x86 binary anywhere
in the corpus, so end-to-end behaviour on the other four architectures remains
unmeasured. What changed is that all five now have their instruction semantics
checked on every push, which is the standing x86 already had and the other four
never did.


## 0/40: what the other four architectures actually did

**Two root causes fixed; the rate itself comes from CI.** ARCH-01's first run,
ctest-linux 285 at 69624d4, is the number the previous entry could not supply:

```
ARCH-01: arm        0/10   0.0000
ARCH-01: arm64      0/10   0.0000
ARCH-01: mips       0/10   0.0000
ARCH-01: powerpc    0/10   0.0000
ARCH-01: overall    0/40   0.0000
```

Zero, on every architecture, in the same run where CC-01 reports 216/216 for
x86-64. Not "worse than x86-64" — nothing at all. Both causes are single
defects, and both are invisible to a corpus that is entirely 64-bit x86.

### Thirty of forty: a constant fold that cannot represent a negative number

arm, mips and powerpc all dumped core in the decoding phase, at the same
assertion:

```
APInt.h:127: llvm::APInt::APInt(unsigned, uint64_t, bool, bool):
  Assertion `llvm::isUIntN(BitWidth, val) && "Value is not an N-bit unsigned value"' failed.
  #13 SymbolicTree::_simplifyNode()  symbolic_tree.cpp:418
  #22 Decoder::getJumpTarget()       decoder.cpp:926
```

`_simplifyNode` folds constant arithmetic with

```cpp
value = ConstantInt::get(c1->getType(), c1->getSExtValue() + c2->getSExtValue());
```

`getSExtValue()` widens to `int64_t`, and `ConstantInt::get`'s `uint64_t`
overload asserts that the value fits the type as an **unsigned** N-bit number.
Add two negative 32-bit constants and the sum is a negative `int64_t`, which as
a `uint64_t` is enormous and does not fit `i32`.

**On a 64-bit target every `int64_t` fits `i64`, so the assertion can never
fire.** That is the whole reason 216 x86-64 binaries never found it and all
thirty 32-bit ones die on it — and jump-target simplification is on the path
of every binary, so it is not an edge case on those targets, it is all of them.

Six folds (add, sub, or, and, the nested add, and the global-address add) use
`APInt` arithmetic now, which is modular at the operand width — what the
machine does, and what cannot assert. The global-address fold also masks the
image address to the pointer width before widening, because there the two
sides genuinely differ.

### Ten of forty: one missing vector arrangement

Every ARM64 binary failed with

```
[capstone2llvmir]: Arm64: extractVectorValue(): Unknown VESS type
```

`extractVectorValue` switches on the vector arrangement specifier and throws on
anything it does not name. Nothing catches that throw, so it ends the
decompilation of the **whole binary** rather than of one instruction. Of the
fifteen `ARM64_VAS_*` capstone 5.0.9 defines, fourteen were named. The missing
one was `ARM64_VAS_2D` — two 64-bit lanes, which is what every double-precision
SIMD operand looks like and which glibc's string and math routines are full of.

It does **not** share the `1D` path, and the first attempt at this did, which
was wrong: `1D` ends in a bitcast to `double`, so routing `2D` through it made
`add v0.2d, ...` an integer add on a double and every ARM64 binary failed with
*"Tried to create an integer operation on a non-integer type"* instead. A `.2d`
lane is 64 bits and the arrangement alone does not say whether the instruction
reads it as an integer or a double, so it returns the integer lane — the choice
`VAS_INVALID` already makes for `vN.d[i]`.

A C enum switched on with a `default` gets no `-Wswitch` help, so C2L-01 now
checks the exhaustiveness textually: every `ARM64_VAS_*` in capstone's header
must appear in `arm64.cpp`. Falsified by deleting the new case.

### Instruction coverage, and why the table ratio is the wrong number

`scripts/ci/check_instruction_coverage.py` (COV-01) disassembles the corpus and
asks what fraction of the instructions **actually present** each translator
implements:

| arch    | decoded   | skipped | rate   |
|---------|----------:|--------:|-------:|
| arm     | 1,571,329 | 474,577 | 0.8981 |
| arm64   | 3,085,112 |      36 | 0.9826 |
| mips    | 3,683,664 |      72 | 0.9952 |
| powerpc | 3,981,728 |   8,212 | 0.9942 |

This inverts the picture the dispatch tables give. By table ratio MIPS looks
worst at 22.6%; weighted by what binaries contain it is the **best** of the
four, at 99.52%. x86 is the lowest of all five by table ratio (24.6%) because
it lists every SSE and AVX form it declines. A ratio over an ISA's own
instruction count is not comparable across ISAs and should not be used to
decide anything.

`skipped` is the honesty column. ARM's 474,577 is static glibc interleaving ARM
and Thumb while this disassembles in one mode: the Thumb regions decode as
garbage, much of it as coprocessor instructions that are not in the binary at
all — which is why ARM's top-uncovered list is led by `STC`, `LDC` and `CDP`,
and why **the ARM figure is a lower bound with a wide error bar and its
uncovered list must not be used to choose what to implement.**

### What the arm64 measurement did justify

Two families, both confirmed present in real binaries before any code changed:

  * The six bitfield-move aliases — `UBFX`, `UBFIZ`, `SBFX`, `SBFIZ`, `BFI`,
    `BFXIL`, 7,526 occurrences — were all `nullptr`. Capstone reports these
    rather than the `UBFM`/`SBFM`/`BFM` they alias (zero occurrences of those),
    so the aliases are what reaches real code. Each is a shift and a mask.

  * `BTI`, `PACIASP`, `AUTIASP` and the rest of the ARMv8.3/8.5 control-flow
    integrity set — 9,829 occurrences — had **no entry at all**, not even
    `nullptr`, because the table predates both extensions. Current toolchains
    emit them unconditionally, so almost every function prologue carried a
    pseudo-call. They model as nothing: BTI is a landing pad, and PACIASP/
    AUTIASP are a sign/authenticate pair whose composition is the identity on
    X30.

arm64 coverage moved 0.9770 → 0.9826 on the same corpus, which is the only
reason to believe the two families were worth the change. Nine tests, each
failing when the dispatch entries are reverted; keystone 0.9.2 predates ARMv8.3
and cannot assemble those mnemonics, so those three go in as encodings checked
against capstone.


### One fix is not one site: the same assertion, four places

Fixing `symbolic_tree.cpp:418` did not move ARCH-01 off 0/40. It moved the
crash. Run 286, at `fcd4895`, still lost all forty — and the failure logs show
the same `isUIntN` assertion firing from three *different* places, one per
architecture, each further down the pipeline than the last:

| arch    | site                                        | reached from |
|---------|---------------------------------------------|--------------|
| mips    | `symbolic_tree.cpp:399`                     | StackAnalysis |
| powerpc | `strength_reduction.cpp:146`                | strength reduction |
| arm     | `fileimage.cpp:143`                         | image reads |

They are all one class — `ConstantInt::get(Type*, uint64_t)` asserts rather
than truncating when the value does not fit the type as an unsigned N-bit
number — and all four instances are shapes that only a 32-bit target reaches:

  * `~((1ULL << n) - 1)` is every high bit set **in sixty-four bits**, so it
    fits no narrower type. (Also undefined for `n >= 64`.)
  * an image address, or a function entry address, written into whatever type
    the load had.
  * `ci->getZExtValue() - 1` where `ci` is zero: `0 - 1` as a `uint64_t` is
    every bit set. Not yet observed, found by looking for the shape.

A sweep of all 457 `ConstantInt::get` call sites in `src/` narrowed to 50 whose
value is an address, a mask, or a shift expression; 32 of those are in the x86
translator, where the constant is built at the operand's own width and which
216 binaries of evidence say is fine. Of the remaining 18, four were wrong and
are fixed; the rest write an address into a default type that is wide enough
for it.

**The lesson recorded rather than the fix.** Each of these was hidden behind
the one before it. A single ARCH-01 number cannot distinguish "one defect" from
"four in a row", and three CI round-trips at forty-five minutes each is what
finding them one at a time costs. The sweep is the answer to that, not another
round trip.

### ARCH-01's 0/40 was two different failures, and one of them was the corpus

Run 287, at `784324e`, still read 0/40 — but the forty logs no longer say the
same thing, and that is the finding.

**Thirty of them are no longer crashes.** ARM, MIPS and PowerPC now run the
entire pipeline. `binary_search-mips-gcc-O0` reaches *Disassembly generation*
at 57.88 s and is killed by ARCH-01's 60-second timeout; PowerPC and ARM get
to *Simple types recovery* at 57.65 s and 59.72 s. The four `ConstantInt::get`
fixes did what they were supposed to do. What stops those thirty is wall clock.

**And the wall clock is my own doing.** `build_multiarch_corpus.sh` passed
`-static`, on this reasoning, written in its own header:

> a dynamically linked cross binary leaves the algorithm in a PLT stub and the
> decompiler with almost nothing to recover

That is wrong, and one command falsifies it: `aarch64-linux-gnu-nm` on a
dynamically linked cross build finds `bubble_sort` and `main` in the binary.
Only the libc calls go through the PLT — exactly as in
`tests/algorithm_recovery/corpus`, the x86-64 corpus the F1 gate scores every
run and which is built with no `-static` at all. What `-static` added was the
whole of glibc: 400–700 KB per binary against 8–70 KB.

So ARCH-01 was not measuring what it claimed. Its question is whether the other
four architectures reach x86-64's result, and it was handing them a
categorically harder input than the x86-64 gates get. The builder now defaults
to the same flags as the x86-64 corpus (`-O0 -fno-inline -fno-builtin`,
dynamic), and `--link static` keeps the harder set for deliberate use — it is
worth keeping, because glibc's hand-written SIMD is what surfaced every bug in
this section.

The claim is now checked rather than argued: the builder fails if `main` is not
a defined text symbol in every binary it produces.

What that changes, measured (COV-01 on the dynamic corpus, against the same
tool's numbers for the static one):

| arch    | static          | dynamic |
|---------|-----------------|---------|
| arm     | 0.8981          | 0.9581  |
| arm64   | 0.9826          | 0.9998  |
| mips    | 0.9952          | 1.0000  |
| powerpc | 0.9942 → 0.9978 | 1.0000  |

(PowerPC's static figure is before → after the load/store work below. Nothing
in this batch changes any dispatch table for the other three, so their two
columns differ only by corpus. Both ARM figures are lower bounds with a wide
error bar — static glibc interleaves ARM and Thumb, COV-01 disassembles in one
mode, and the skipped-byte column says so: 474,577 static, 426 dynamic.)

Instruction coverage on binaries comparable to the x86-64 corpus is already
essentially complete for three of the four. Whatever ARCH-01 finds next, it is
not mostly a coverage problem.

### ARM64: an integer add on a float, in all ten binaries

The tenth of ARCH-01's forty is a different failure, and a real translator bug.
All ten ARM64 binaries die in the same place:

```
Assertion `getType()->isIntOrIntVectorTy() && "Tried to create an integer
operation on a non-integer type!"' failed.
  ... Capstone2LlvmIrTranslatorArm64_impl::translateAdd (arm64.cpp:1255)
```

`add v0.4s, v1.4s, v2.4s` is four 32-bit adds. Two things have to be wrong for
it to reach `CreateAdd`:

1. `extractVectorValue` extracts a lane even when there is no lane to extract.
   A whole-register operand carries an arrangement and `vector_index == -1`;
   every branch of that switch multiplies the index by a lane width and shifts
   by the result, so -1 became a shift by 2^128−32 — poison — then a truncation
   of the poison to a lane type. For the S arrangements that type is `float`.
2. `translateAdd` bitcasts FP operands back to integers only when
   `isFPRegister(operands[0])`, and that predicate knows Q, D, H and S
   registers but not V. So the bitcast never fired.

Both are fixed. A negative index now returns the register whole, which is also
exactly right for the bitwise vector instructions (`eor`/`and`/`orr`/`mov` on
`.16b` are lane-agnostic), and `translateAdd`/`translateSub` take the
`ifVectorGeneratePseudo` guard that five other translators in the same file
already use — a lanewise add is not a 128-bit add, and saying "not modelled" is
better than emitting the wrong arithmetic.

**Reproduced without an assertions build.** The distribution LLVM is compiled
with `NDEBUG`, so the assertion cannot fire here. The regression tests do not
depend on it: they read the emitted IR and fail on an integer opcode carrying a
floating-point type, and on a shift by a constant wider than 64 bits. Reverting
either fix makes them fail with exactly the defect the CI log named.

### PowerPC had no floating point at all

Every one of the 44 `PPC_INS_F*` entries in the dispatch table was `nullptr`,
and so were all sixteen float load/store forms — while `powerpc_init.cpp` maps
`PPC_REG_F0..F31` to `double`. The register file was modelled and nothing ever
wrote to it. COV-01 put `STFD` and `LFD` at the top of PowerPC's untranslated
list, 0.37% of every instruction in the corpus between them.

This is not a coverage statistic. A float load that becomes
`call @__asm_lfd(...)` severs the dataflow the rest of the decompiler runs on,
and the store form was worse: capstone marks the memory operand as read, so the
generic pseudo-asm fallback emitted a *load* from the address a `stfd` stores
to.

Fixed for the loads and stores — `LFS LFSU LFSX LFSUX LFD LFDU LFDX LFDUX` and
the eight `STF` forms — and for the 64-bit integer forms, which were equally
absent and are every pointer load in a ppc64 binary: `LD LDU LDX LDUX STD STDU
STDX STDUX LWA LWAX LWAUX`. PowerPC's corpus coverage goes 0.9942 → 0.9978
(1.0000 on the dynamic corpus), and its suite 808 → 834 tests.

The arithmetic — `FADD FSUB FMUL FDIV FMR FABS FNEG FCMPU`, the `FMADD`
family, the conversions — is still `nullptr`. It is the next piece of work, not
a thing this section claims to have done.

**Not doing, and why it would be cheating.** `SC`, `MFFS`, `TRAP`,
`DCBST`/`ICBI`/`DCBZ`, MIPS `RDHWR` (0.45% of MIPS, the single largest
untranslated entry it has), ARM64 `MRS` and `SVC` all already reach
`translatePseudoAsmGeneric`, which reads capstone's per-operand access flags
and emits exactly the call an explicit pseudo-asm wiring would. Pointing their
table entries at a helper would move roughly 1% of MIPS and 0.5% of ARM64 out
of COV-01's untranslated column without changing one instruction of output.
That is moving the metric, not the product.

### 32-bit ARM had no floating point either, and one entry was unreachable

The last of the five. All 149 `ARM_INS_V*` entries were `nullptr`, so every
floating-point instruction on 32-bit ARM was an opaque `__asm_*` call -- while
`arm_init.cpp` models 32 S registers as f32 and 32 D as f64. A six-line dot
product built for `arm-linux-gnueabihf` emits `vldr`, `vstr`, `vmul.f64`,
`vadd.f64` and `vmov.f64` in one function, none of which reached the rest of
the decompiler.

Scalar VFP is now translated: VLDR VSTR VADD VSUB VMUL VDIV VNMUL VNEG VABS
VSQRT VMLA VMLS VNMLA VNMLS VFMA VFMS VFNMA VFNMS VCMP VCMPE VCVT VCVTR VMOV
VMRS. NEON is not, and says so: anything whose `cs_arm::vector_data` is an
integer type, or whose operands are Q registers or carry a lane index, goes to
`translatePseudoAsmGeneric` -- the answer arm64 already gives through
`ifVectorGeneratePseudo`. A lanewise add is not a scalar add.

**The operand shapes were measured, not assumed.** Encodings taken from a real
`arm-linux-gnueabihf-gcc -O0` build and run through the capstone revision
`cmake/deps.cmake` pins. Three would have been wrong:

  * `vcmp.f64 d7, #0` gives an **IMM 0**, not an FP operand, so the zero has to
    be built from the other operand's type.
  * `vmov d6, r0, r1` is low-word-first -- confirmed from the `ldrd r0, r1` and
    `__floatdidf` call that precede it in real output.
  * `vmrs`'s operands are both marked `access=0`, so the access flags cannot
    drive it.

**And one entry could never have fired.** Capstone 5.0.9 decodes
`vmrs APSR_nzcv, fpscr` as `ARM_INS_FMSTAT` (58), the pre-UAL alias, while
printing the mnemonic as "vmrs". `ARM_INS_VMRS` is 378 and is the general
`vmrs rN, fpscr` form. The ARM table had no key for 58 at all, so wiring
`ARM_INS_VMRS` looked exactly like a fix and did nothing -- which is what it
did, until the test said so.

This is the same shape as the duplicate `ARM64_INS_HINT` key: a dispatch entry
that is never reached is dead, and it reads as done. C2L-01 guards the
duplicate case textually; this one is only catchable by assembling the
instruction and asserting the translation, which is what the 40 new ARM tests
do. A count of what each table omits, for the record:

| arch    | table keys | capstone ids | absent |
|---------|-----------|--------------|--------|
| arm     | 437       | 471          | 36     |
| arm64   | 468       | 1288         | 821    |
| mips    | 627       | 625          | 0      |
| powerpc | 1192      | 1726         | 536    |
| x86     | 1343      | 1523         | 182    |

Most of those are unreachable-in-practice (arm64's are SVE and SME, which the
table predates) and all of them still reach the pseudo-asm fallback, so an
absent key is latent rather than broken. It becomes a defect the moment someone
wires the canonical name for an encoding capstone reports under an alias.

**Verified on the floating-point binaries**, which is the only evidence that
counts here: COV-01 over cross-built float programs reports powerpc **1.0000**,
mips 0.9916 (`MTHC1`), arm64 0.9846 (`LD1`/`ST1`). The 32-bit ARM figure cannot
be read the same way -- COV-01 disassembles in one mode and this corpus is
Thumb -- so ARM's evidence is the 40 tests, which run in both CS_MODE_ARM and
CS_MODE_THUMB.

### Six sources, so that the floating-point work is measured at all

The section below records that no corpus source contained a `float` or a
`double`. Six now do, and they were chosen so that between them they emit the
instructions the last three commits implemented:

| source | what it exercises |
|--------|-------------------|
| `float_dot_product` | double load, multiply, accumulate, return |
| `float_mean_variance` | division by a converted int, nested calls |
| `float_newton_sqrt` | an iterative loop, compare against a constant |
| `float_matrix_multiply` | `float` rather than `double`, 2-D indexing |
| `float_compare_sort` | floating-point comparison driving control flow |
| `float_int_conversion` | every direction of int/float conversion |

No libm: a call to `sqrt()` would land in a PLT stub and say nothing about a
translator, so Newton-Raphson is written out in multiplies and divides.

Across the six, PowerPC emits `lfd` (84), `stfd` (34), `fsub` (16), `fctiwz`
(16), `fmr` (13), `fcmpu` (11), `fmul` (10), `lfs` (8), `stfs` (6), `fneg`,
`fadd`, `frsp`, `fmadd`, `fdiv`, `fadds`, `fmuls`; 32-bit ARM emits `vldr`
(40), `vmov` (39), `vcvt` (21), `vstr` (15), `vmul`, `vadd`, `vmrs`, `vcmpe`,
`vneg`, `vdiv`, `vsub`, `vmls`. Every one of those was an opaque `__asm_*`
call at the start of this session.

COV-01 over the cross-built set: **powerpc 1.0000, mips 1.0000**, arm64 0.9958
(`LD1`/`ST1`, NEON, out of scope by design).

What this costs, and what it risks: the x86-64 corpus goes 216 -> 252 binaries
and the multiarch one 144 -> 168. CC-01's floor is a rate, so the count moving
does not touch it -- but its full run now covers floating-point programs for
the first time, and if their emitted C does not compile the gate goes red. That
would be the finding, not a regression.

ARCH-01's floored step takes `--limit 40`, the first ten programs by name, and
the float sources sort past that window. So a second step runs the whole 168
and reports without a floor -- the road CC-01 took from 24 binaries to 252, and
that ARCH-01 itself took from 0/40 to 40/40. A floor picked before the first
number is either vacuous or a fiction.

### Two instructions the IR-to-HLL converter aborts on

The whole-corpus ARCH-01 step existed to measure the 128 binaries the floored
one never sees. Its first run, ctest-linux 290 at `938f6be`, read **162/168**,
and the six failures are two decompiler crashes:

```
ARCH-01: arm       41/42   0.9762
ARCH-01: arm64     40/42   0.9524
ARCH-01: mips      41/42   0.9762
ARCH-01: powerpc   40/42   0.9524
ARCH-01: overall  162/168  0.9643
```

| instruction | failures | why it is there |
|-------------|----------|-----------------|
| `fneg`      | 4 — arm, arm64, mips, powerpc | LLVM 11 replaced the `fsub -0.0, x` idiom with a dedicated unary operator; this converter predates it |
| `freeze`    | 2 — arm64, powerpc | LLVM's own optimiser inserts it; no translator chose to emit it |

Both reach `LLVMInstructionConverter::visitInstruction`, which calls `FAIL()`
and aborts the process. Not a wrong result — the whole decompilation ends.

**`fneg` is not new, and not from this session's work.** `git blame` puts the
five `CreateFNeg` calls in `arm64.cpp` at `894733d`, the initial import. Any
ARM64 binary containing a floating-point negation has ended the decompiler
since then. Nothing caught it because not one of the 367 corpus sources
contained a `float` — which is exactly what the six added in the previous
commit were for, and they found it on the first run.

The conversions are both one line of meaning. `fneg x` is `NegOpExpr(x)`.
`freeze x` is `x`: it pins any undef or poison in its operand to a value the
instruction does not name, and a decompiler has no undef to pin, so there is
nothing to emit but the operand.

**`gcd_euclid` is the `freeze` one, and it is an integer program.** So this was
never a floating-point problem; widening the measurement is what found it. The
floored ARCH-01 step takes the first ten programs by name and would not have
reached either binary.

### A gate that reused objects it could not identify

Diagnosing the two above cost a stash-and-rebuild, because L2H-01 reported

```
undefined reference to llvm::APInt::toString(SmallVectorImpl<char>&, unsigned, bool, bool, bool)
```

from `const_int.cpp` — a file nothing in the working tree had touched. It was
not the working tree. `L2H_WORKDIR` reuses an object whenever the object is
newer than its source, which says nothing about the compiler or the LLVM
headers that produced it, and the directory I pointed it at held objects dated
six days earlier, built against a different LLVM. `APSInt::toString` is an
inline; the stale `const_int.o` carried a call to the five-argument
`APInt::toString` that LLVM 20 does not export, and the linker only complained
once something referenced that weak section.

The flags, the compiler and the LLVM version are stamped into the workdir now,
and a workdir stamped differently is emptied rather than reused.

**The first version of that guard missed its own motivating case.** It trusted
a workdir carrying no stamp — and the directory that caused the trouble
predated stamping, so it carried none. Run against it, the guard passed the
stale objects straight through and reproduced the identical link error. An
unstamped workdir is now discarded too: its provenance is unknown, which is
precisely what the check refuses. That is the third time in this work a checker
turned out to be more permissive than the thing it stood in for.

Two other notes from the same episode, neither worth a guard but both worth
not repeating:

  * Editing a shell script while it is running corrupts that run. Bash reads a
    script incrementally, so an edit shifts the bytes under the interpreter;
    this one died on `STAMP: unbound variable` at a line that defines `STAMP`
    twenty lines earlier.
  * The evidence that the working tree was innocent came from `git stash` and a
    clean rebuild — 2207 tests, linking fine — not from reading the diff and
    deciding it looked unrelated.

### 32-bit ARM claimed a capacity it had no registers for

`arm_conv.cpp` set

```cpp
_numOfFPRegsPerParam = 2;
_numOfVectorRegsPerParam = 4;
```

and declared neither `_paramFPRegs` nor `_paramDoubleRegs` nor
`_paramVectorRegs` — the only one of the eleven conventions that named none of
them. AAPCS-VFP passes floating-point arguments in s0–s15 and d0–d7 and returns
in s0 or d0, and this project's ARM corpus is hard-float, so every
floating-point parameter and return value on 32-bit ARM was invisible to
parameter recovery.

Nothing failed. `usesFPRegistersForParameters()` returns false when both lists
are empty, so the analysis never looked — while the two "how many registers may
one parameter span" numbers sat there reading as though the subject had been
thought about.

**Both lists, and why that is enough.** The blocker I recorded earlier was that
`arm_init.cpp` gives the S and D registers separate globals instead of
modelling them as one bank, so naming either alone catches only half the ABI.
MIPS has the identical split and solves it by declaring both `_paramFPRegs`
(single) and `_paramDoubleRegs` (double); ARM now does the same. The aliasing
that is still not modelled would matter only if a value were written as S and
read as D, which compiler output for a single argument does not do.

**And the hard/soft question does not block it either.** On soft-float ARM
floats pass in r0–r3. This analysis is driven by which registers a callee
actually *reads* before writing, so a soft-float function simply never reads
s0 and nothing is reported: the lists add candidates, they do not assert an
ABI. Reading `EF_ARM_ABI_FLOAT_HARD` would be the faithful thing and nothing
reads it today — `ElfFormat::getAbiVersion()` reads `EF_ARM_ABIMASK` and stops
there — but it is not needed to make this correct.

**What no gate here checks.** Nothing in this repository measures parameter
recovery on ARM: the F1 gate is x86-64 and nine binaries, and ARCH-01 checks
that decompilation exits 0, not what it recovered. So this change is a
declaration matching a published ABI, verified to compile and following the
MIPS precedent — and that is the whole of the evidence for it.

What *is* checked is the shape of the mistake. CC-CONV fails any convention
that sets `_numOfFPRegsPerParam` without declaring FP or double registers, or
`_numOfVectorRegsPerParam` without vector ones. Falsified against the real
pre-fix file, which it rejects with both rules.

### What is left on the other three, measured

COV-01 over the whole 168-binary multiarch corpus, floating-point sources
included:

| arch    | rate   | what is left |
|---------|--------|--------------|
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |
| arm64   | 0.9993 | `LD1`/`ST1` (NEON, 2 each) and `LDADDAL` (1) |
| arm     | 0.9517 | 536 skipped bytes; see COV-01's own header |

ARM's figure is not comparable and the tool says so: static and Thumb-mode code
disassembled in a single mode decodes as garbage, which is why its
uncovered list is led by `STC`, `CDP` and `LDC` -- coprocessor instructions
that are not in the binary at all.

`LDADDAL` is worth naming because it is not NEON and not a misdecode. The
ARMv8.1 LSE atomics -- `LDADD`, `LDCLR`, `LDEOR`, `LDSET`, `SWP`, `CAS` and
their acquire/release forms -- have **no entry at all** in the ARM64 table, not
even `nullptr`, while `LDAXR`/`STLXR` are implemented. Modern glibc uses LSE
where the target supports it. One occurrence in this corpus, so it is recorded
rather than fixed; `atomicrmw` is the obvious mapping and the existing
`setAtomic` calls show the shape.

### MIPS: the other half of the move

`mtc1` and `mfc1` move the low half of a 64-bit FPU register. `mthc1` and
`mfhc1` move the high half, and a compiler emits them in pairs to get a double
into and out of the FPU without going through memory. Both were `nullptr`, and
`MTHC1` was the only instruction COV-01 found untranslated in MIPS
floating-point programs.

The double lives in the `FDn` register that
`singlePrecisionToDoublePrecisionFpRegister` maps `$fN` onto, so the fix reads
that register's bits, replaces one half and writes it back. The low half
surviving is the whole point, so the test starts from `0x3ff0000012345678` --
a non-zero low half, which the obvious wrong implementation (store the shifted
high word) would clobber. Falsified exactly that way.

Keystone 0.9.2 will not assemble `mthc1` in its MIPS32 mode ("instruction
requires a CPU feature not currently enabled" -- it is MIPS32r2), so both go in
as encodings checked against capstone 5.0.9 first. MIPS coverage on
floating-point binaries: 0.9948 -> **1.0000**.

### ARCH-01: 40/40, and the floors go in

Run 288 at `517db16`:

```
ARCH-01: arm       10/10   1.0000
ARCH-01: arm64     10/10   1.0000
ARCH-01: mips      10/10   1.0000
ARCH-01: powerpc   10/10   1.0000
ARCH-01: overall   40/40   1.0000
```

against CC-01's 216/216 for x86-64 in the same run. Three commits earlier it
was 0/40 on every architecture. The four defects between those two numbers:

| what | where |
|------|-------|
| `ConstantInt::get(Type*, uint64_t)` asserts instead of truncating | four sites, each hidden behind the last |
| `ARM64_VAS_2D` missing from a switch with a throwing default | `extractVectorValue` |
| a lane extracted from a whole-register operand, yielding a `float` to an integer add | `extractVectorValue`, `translateAdd` |
| `-static`, which made the corpus a harder problem than the one x86-64 is asked | `build_multiarch_corpus.sh` |

Only the first was visible as "the decompiler is broken". The second and third
needed the failure logs read rather than the wall clock; the fourth was mine,
and needed the premise checked rather than believed.

The step is a floor now, per architecture and set at what was measured, which
is the road CC-01 took from 0/216 to 216/216. Per architecture and not in
aggregate: an overall floor of 1.0 would pass while one architecture went to
zero and another gained the same count, which is exactly what a parity gate
must not do.

### PowerPC floating-point arithmetic, and the emulator that could not run it

The load/store half landed first. This is the other 44 entries: `FADD FSUB
FMUL FDIV` and their single-precision forms, `FMR FNEG FABS FNABS FSQRT FRSP
FCPSGN FSEL`, the `FMADD` family, `FCMPU`, the estimates (`FRE FRSQRTE`), the
rounding forms (`FRIM FRIN FRIP FRIZ`) and the twelve integer conversions.
PowerPC's dispatch table now has no `PPC_INS_F*` left at `nullptr`.

Three things worth writing down.

**`FCMPU` cannot use `storeCrX`.** That helper builds integer comparisons, and
it writes a constant zero into the fourth bit of the CR field because for the
integer compares that bit is a copy of XER, which is not modelled. For a
floating-point compare the fourth bit is FU, "unordered", and it is the only
thing that distinguishes comparing a NaN from comparing two equal numbers. So
`translateFcmp` writes the field itself, from `fcmp olt/ogt/oeq/uno`.

The `crReg -> {lt, gt, eq, so}` switch was written out three times in
`powerpc.cpp` already. A fourth copy was not the answer; `crFieldRegisters()`
is, and the existing three now call it.

**The single-precision forms round, and my first test could not tell.** An `S`
form computes in double and rounds the result to single before writing it
back. The obvious test value is 0.1, whose double and float-rounded-to-double
representations differ by about 1.5e-9 — and this harness compares doubles to a
tolerance of 0.001, so that test passed whether the rounding happened or not. I
found this by deleting `roundToSingle` and watching the test still pass. The
value is 2^24+1 now: the smallest integer a float cannot represent, which
rounds to 2^24, a difference of 1.

**The emulator could not execute any of it.** `tests/capstone2llvmir` runs the
IR it produces, and `src/llvmir-emul` handled none of the floating-point
intrinsics:

  * `IntrinsicLowering` turns most of them into libcalls — `sqrt`, `floor`, … —
    which the interpreter cannot resolve, so `fsqrt 16.0` came back 0.
  * For `llvm.fma` it does not even do that. It reports "Code generator does
    not support intrinsic function" through `report_fatal_error`, which ends
    the process: one `fmadd` test killed the whole 4,477-test binary.
  * `llvm.fabs`, `llvm.minnum` and `llvm.maxnum` were already excluded from
    lowering, with the comment "can not lower those functions", and then fell
    through to the unhandled-external path — where a `double` return leaves
    `GenericValue::DoubleVal` uninitialised. They were returning whatever was
    on the stack, and the ARM64 and x86 tests that use them only ever asserted
    that the call was *recorded*, never what it returned.

`visitCallInst` now computes fabs, sqrt, floor, ceil, trunc, round, roundeven,
nearbyint, rint, copysign, minnum, maxnum and fma directly, for f32 and f64.
Not for x86_fp80: `GenericValue` keeps those in `IntVal`, not `DoubleVal`, so
the fp80 forms keep the path they had. And it still records the call, because
six ARM64 and x86 tests assert exactly that — which is how I found out I had
broken them.

Eleven tests in `tests/llvmir-emul`, under a class name the C2L-01 filter
actually matches (`LlvmIrEmul*`). The first draft called it `FpIntrinsicTests`,
which the gate's own filter would have skipped: eleven tests that could
disappear without the floor noticing.

### Not one of the 367 corpus sources contained a float

```
$ find tests/algorithm_recovery/sources -name '*.c' | wc -l
367
$ find tests/algorithm_recovery/sources -name '*.c' | xargs grep -lE '\b(float|double)\b' | wc -l
0
```

CC-01, DET-01, the F1 recovery gate and ARCH-01 all run on this corpus. None of
them has ever put a floating-point instruction through the decompiler — on
*any* architecture, x86-64 included. "216/216 emitted C files compile" is 216
integer programs.

That is the honest limit on the PowerPC floating-point work above: it is
covered by 834 unit tests in `tests/capstone2llvmir`, and by no end-to-end gate
at all, because no end-to-end gate has anything to run it on.

Two related gaps, found while checking whether the FP work would even reach the
emitted C:

  * **The PowerPC path is otherwise complete.** `powerpc_conv.cpp` already
    lists `_paramFPRegs` F1–F8 and `_returnFPRegs` F1. The machinery was
    waiting for values that no instruction produced.

  * **32-bit ARM has no FP registers in its calling convention at all.**
    `arm_conv.cpp` sets `_numOfFPRegsPerParam = 2` and
    `_numOfVectorRegsPerParam = 4` and then declares neither `_paramFPRegs` nor
    `_returnFPRegs` — the only one of the eleven conventions that does not.
    `arm_init.cpp` models `ARM_REG_S0` as f32 and `ARM_REG_D0` as f64, so the
    registers exist. The corpus is built for `arm-linux-gnueabihf`, whose ABI
    passes floats in s0–s15 and returns in s0, so on that ABI every float
    parameter and return is currently invisible to parameter recovery.

    Not fixed here, and not a one-line fix: `arm_init.cpp` gives S and D
    registers separate globals rather than modelling the aliasing, so choosing
    `_paramFPRegs = {S0..S15}` or `{D0..D7}` decides which half of the ABI
    works. And which list applies at all depends on EF_ARM_ABI_FLOAT_HARD in
    the ELF header, which nothing reads. Writing the register list without
    settling those two would be guessing.

## The "nobody builds this" check never asked it about src/

**Closed by extending `check_cmake_sources.sh`.** The check exists to catch
sources no CMakeLists names — its own comment says *"a check written to catch
'nobody builds this' that skips the directories nobody builds is worse than
none, because its OK is trusted"* — and it walked `tests/` only. `src/` was
never asked the question.

Found while chasing the `ConstantInt::get` class: `types_propagator.cpp`
appeared in a compile sweep with

```
error: 'resolveTypes' was not declared in this scope
error: no declaration matches 'bool TypesPropagator::resolveTypes()'
```

which cannot compile on any compiler — and CI builds bin2llvmir fine, because
nothing compiles that file.

Three sources under `src/` are named by no CMakeLists, and the check now
carries each with its reason:

  * `unpackertool/plugins/example/example.cpp` — a template for writing an
    unpacker plugin. Compiles; building it would register an unpacker that
    unpacks nothing. Legitimately unbuilt.
  * `rtti-finder/vtable/vtable_xref.cpp` — 244 lines, **does not compile**: 14
    errors against the current API, including `VtableGcc::virtualFunctions`,
    which does not exist. Its header is declared and included by nothing.
  * `bin2llvmir/optimizations/types_propagator/types_propagator.cpp` — 347
    lines, **does not compile**, as above. `simple_types/` is the types pass
    the build actually uses.

591 lines across the two broken files, neither of which has ever been through
a compiler. They are recorded rather than repaired: making dead code compile is
polish on something nothing runs, and wiring an unverified optimisation pass
into the pipeline is a behaviour change with no evidence behind it. What is
fixed is that the next one cannot arrive unnoticed — falsified two ways, by
adding an unnamed source under `src/` and by deleting a reason from the list.


## bin2llvmir's "real drift across many files" was one API in seven files

The largest ungated suite carried this reason:

> uses LLVM 20+/21+ APIs (CmpPredicate, Intrinsic::getOrInsertDeclaration,
> Value::hasUseList); measured against system LLVM 18, it is real drift across
> many files, not a missing -I

Measured against `llvm-20-dev` rather than 18, it was 8 files of 125. One of
those was a missing `-Ideps/eigen`; one was `types_propagator.cpp`, which is
built by nothing and does not compile at all. The other six, plus one more,
were a single API: `Value::hasUseList()`, which LLVM 21 added when it stopped
giving every Value a use list and made `users()` assert for the ones without.

`llvm_utils::hasUseList()` spells that for both — `hasUseList()` from 21, and
`true` before it, where every Value had a use list and `users()` asserted for
none. The shipped build takes the first branch, so this is a no-op there.

With it: **all 124 buildable `src/bin2llvmir` sources compile against LLVM 20,
and 30 of the 31 in `tests/bin2llvmir`.**

**What still blocks the suite, precisely.** `src/debugformat/dwarf.cpp`
includes `llvm/DebugInfo/DWARF/LowLevel/DWARFExpression.h`, which exists only
from LLVM 21, and `DebugFormat::loadDwarf()` is referenced by the link — so the
test binary cannot be linked here even though almost everything compiles.
Separately, `tests/bin2llvmir/utils/simplifycfg_tests.cpp` `#include`s an LLVM
*source* file (`../lib/Transforms/Scalar/SimplifyCFGPass.cpp`) from the
vendored llvm-project tree, so it only builds inside the full build.

That is worth the distinction. "Real drift across many files" is a reason to
stop looking; "one file needs one LLVM 21 header" is a reason to keep going,
and it is what the measurement says.


## The 124 ARM64 atomics that were not `nullptr` — they were absent

COV-01's arm64 leftovers named `LDADDAL`, and the entry above recorded it
rather than fixing it: one occurrence in the corpus. Then I went looking for
the entry to change and there wasn't one. `LDADD`, `LDCLR`, `LDEOR`, `LDSET`,
`LDSMAX`, `LDSMIN`, `LDUMAX`, `LDUMIN`, `SWP` and `CAS` — ten operations, four
ordering suffixes each (plain, `A`, `L`, `AL`), three widths each (`B`, `H`,
word/doubleword) — had **no key at all** in `arm64_init.cpp`'s dispatch map.
Not a translator that declined to model them. Ids the dispatch could never
reach, which is a different thing from a `nullptr` and reads differently in a
`grep`: a `nullptr` count says "we know about these"; an absent key says
nothing at all.

`LDAXR`/`STLXR`, the ARMv8.0 load/store-exclusive pair they replace, *are*
implemented. So the gap is specifically ARMv8.1, and a compiler told
`-march=armv8.1-a` or later emits LSE for every atomic in the program. Modern
glibc does this at runtime through ifuncs. On x86-64 the equivalent
instructions — `LOCK XADD`, `CMPXCHG` — translate to `atomicrmw` and
`cmpxchg`, and llvmir2hll converts both into C. This was the same path all
along; only ARM64's use of it was missing.

**The operand order is uniform, and CAS breaks it.** Measured with capstone
5.0.9:

```
op0 = Rs (the value)   op1 = Rt (the destination)   op2 = [Xn]
```

so `ldadd w1, w2, [x3]` reads `w1`, adds it to the memory at `x3`, and writes
the *old* memory value to `w2`. `cas` uses the same three slots for different
roles: `cas Rs, Rt, [Xn]` compares against Rs, stores Rt on a match, and writes
the old value back to **Rs**. The destination is operand 0, not operand 1 — the
opposite of the other nine. That is the sort of thing a family-wide loop gets
wrong silently, so `translateCas` is a separate function.

**Two things do not map straight across.**

`LDCLR` clears the bits set in Rs, which is `And` with the complement, not
`And` — `ldclr` with operand `0xff` clears a byte, it does not keep it. There
is no `AtomicRMWInst::AndNot`, so the value is `CreateNot`-ed first and the op
is `And`.

`CAS`'s failure ordering may not be stronger than its success ordering and may
not be `Release` or `AcquireRelease` at all (LLVM asserts). `CASL` and `CASAL`
therefore take `Monotonic` on the failure path, which is what the architecture
means anyway: a release barrier on a store that did not happen is not a thing.

`CASP`, the 128-bit register-pair form, is deliberately not here. It needs a
register pair on each side and `cmpxchg` takes one value; it falls through to
the pseudo-asm path, as it did before.

**The width comes from the mnemonic, not the register.** `ldaddb w1, w2, [x3]`
names W registers and touches one byte. The first version of the test could not
have caught this getting it wrong, twice:

* First attempt: plain values, so an `i32` RMW and an `i8` RMW at the same
  address computed the same answer.
* Second attempt: a neighbouring `0xff` byte that a 32-bit access would drag
  in — which also passed, because the llvmir-emul memory model is an
  address→`GenericValue` map, not a byte array. Access *width* is not
  observable through that emulator at all, no matter what you put next door.

The test that works asserts on the IR: find the `AtomicRMWInst`, ask
`getValOperand()->getType()->getIntegerBitWidth()`, require 8. Falsified by
returning `getDefaultType()` from `lseAccessType` — 8 vs 32, which is the
failure the first two versions were supposed to produce.

**Keystone cannot assemble any of this.** 0.9.2 is from 2017 and predates
ARMv8.1; it refuses every LSE mnemonic. The encodings are from
`aarch64-linux-gnu-as` and checked against capstone 5.0.9 before use, the same
route the MIPS `mthc1` entry above took.

### Three emulator defects the atomics tests surfaced

`llvmir-emul` is what the capstone2llvmir tests run the translated IR on, so
adding atomics to ARM64 immediately ran into what it does with them.

`atomicrmw` handled `Xchg` and nothing else — every other op fell through to
the same "store the new value" path, so `add`, `and`, `or`, `xor`, `max`,
`min`, `umax` and `umin` all silently behaved like `xchg` and returned the
right *old* value while writing the wrong *new* one. A test that only reads the
result would pass. Now all twelve integer ops compute.

`cmpxchg` was not handled at all.

`extractvalue` returned a default-constructed `GenericValue` — i.e. whatever
was on the stack — instead of the requested element. That one is not
atomics-specific: it is wrong for every aggregate, and it was reachable before
this branch by any IR that used one. `cmpxchg` is what made it load-bearing,
since its result is a `{value, i1}` pair.

### llvmir2hll: four `atomicrmw` kinds that reached the empty statement

The earlier entry on `atomicrmw` conversion wired `add`, `sub`, `and`, `or`,
`xor` and `xchg`, and said the remaining forms "are not lowered and take the
empty-statement path". `LDSMAX`/`LDSMIN`/`LDUMAX`/`LDUMIN` are exactly those
remaining forms, so that path stopped being hypothetical. All four are now
`TernaryOpExpr`s — `a > b ? a : b` with `GtOpExpr::Variant::SCmp` for the
signed pair and `UCmp` for the unsigned — which is what the operation means and
what a reader wants to see.

`Nand` is still not converted, and this is a real limitation rather than an
oversight: BIR has no bitwise-not. `NotOpExpr` emits `!`, the logical one, so
writing `!(a & b)` for `~(a & b)` would be a silent miscompilation of exactly
the kind section 3 is about. It keeps the empty-statement path until BIR grows
the operator.

### What it is gated at

`MIN_ARM64` goes 471 → **481**. The ARM64 suite is the only one that moves.



## The architecture everything else is measured against could not add two doubles

Asked to bring ARM, ARM64, MIPS and PowerPC up to x86-64's level, I spent most
of this branch on those four. Then I went to compare `x86_init.cpp` against
`arm_init.cpp` and found this:

```
{X86_INS_ADDSD, nullptr},
{X86_INS_SUBSD, nullptr},
{X86_INS_MULSD, nullptr},
{X86_INS_DIVSD, nullptr},
{X86_INS_UCOMISD, nullptr},
{X86_INS_COMISD, nullptr},
{X86_INS_SQRTSD, nullptr},
{X86_INS_MAXSD, nullptr},   {X86_INS_MINSD, nullptr},
{X86_INS_CVTTSD2SI, nullptr},
{X86_INS_XORPD, nullptr},   {X86_INS_ANDPD, nullptr},
{X86_INS_MOVSS, nullptr},
```

The whole double-precision half of SSE2. On x86-64 the System V ABI passes and
returns a `double` in an XMM register, and gcc compiles `a + b` on doubles to
`addsd`; there is no other way to write it. So the one architecture with 252
binaries of end-to-end evidence behind it, the control every other number in
this branch is quoted against, turned every floating-point program into a wall
of `__asm_addsd` and `__asm_movsd` calls.

`MOVSD` was worse than `nullptr`. Capstone gives `movsd xmm0, qword ptr [rdi]`
and the string instruction `movsd` **the same id**, and the table pointed that
id at `translateMoveString`, which rejects anything whose operands are not both
memory. There is even a TODO in that function naming the exact address in the
exact sample where somebody hit it. Counted over the six floating-point corpus
programs, `movsd` is the second most frequent instruction in the whole text
section, behind `mov`.

### Why no gate saw it

Three reasons, and each is worth fixing in its own right.

**COV-01 did not measure x86.** Its four architectures were the non-x86 four,
because the tool was written to answer "are the other four as good as x86-64"
and nobody thought to point it at x86-64. Its own header explains at length why
a dispatch-table ratio is the wrong number and a corpus-weighted one is right;
the corpus it weighted by had no x86 in it. It does now:

| arch    | before | after  | what is left |
|---------|--------|--------|--------------|
| x86_64  | 0.9846 | 0.9931 | `HLT` -- one per binary, alignment padding |

**COV-01 cannot see a translator that declines.** Its question is whether the
id has a non-null function pointer, so `MOVSD` counted as covered in both
columns above while producing a pseudo-asm call every time. The 0.9846 was
therefore an overstatement, and the real gap was larger than the table shows.
This is recorded rather than fixed: answering it properly means running the
translator over every decoded instruction and asking whether a `__asm_` call
came out, which is a different tool from a disassembler and a dispatch table.

**Nothing ran COV-01 at all.** It is a script with no caller -- not in
`check_push_gates.sh`, not in any workflow. A measurement nobody takes is a
measurement nobody has.

**`x86_sse.cpp` had no tests.** 668 lines, 24 translators, and the string
`XMM` appeared in `tests/capstone2llvmir/x86_tests.cpp` eight times, all of
them inside comments about `FXSAVE`. Two of those translators --
`translateSsePshufd` and `translateSsePbyteShift` -- had no dispatch entry
either: written, declared, compiled, unreachable. The file's own header says
"the apply script does this automatically via sed", and for those two it did
not.

### What is implemented

The machinery was already there. `scalarFltBinOp` and `packedFltBinOp` both
take an `isDouble` flag and neither had a caller that passed `true`.

* `translateSseFltArith` -- ADD/SUB/MUL/DIV across SS, SD, PS and PD. The
  scalar forms leave the upper lanes alone; that is the entire difference
  between `addsd` and `addpd`, and it is what the tests check.
* `translateSseFltMinMax` -- MAX and MIN. Deliberately not `llvm.maxnum`:
  x86 defines `MAXSD` as `dst > src ? dst : src`, so a NaN in either operand
  yields the **source**, where `llvm.maxnum` returns the non-NaN operand. A
  select spells the architecture's definition exactly and the intrinsic does
  not.
* `translateSseSqrt` -- the one shape nothing else has: the root of the
  *source's* low lane, with the *destination's* upper lanes kept.
* `translateSseComi` -- UCOMIS\*/COMIS\* to EFLAGS, branchlessly. The
  architecture's table is
  `unordered ZF=PF=CF=1`, `less CF=1`, `equal ZF=1`, `greater all clear`,
  which is `ZF = fcmp ueq`, `CF = fcmp ult`, `PF = fcmp uno` and nothing else.
  COMIS\* differs from UCOMIS\* only in which NaNs raise an exception and this
  lifter has no exception state, so they translate the same.
* `translateSseFltLogic` -- AND/ANDN/OR/XOR on all 128 bits. These are in
  every floating-point binary because they are how a compiler writes negation
  and absolute value. ANDN complements the **destination**, the reverse of what
  the operand order suggests.
* `translateSseMovScalar` -- MOVSS and the SSE MOVSD, telling the SSE form from
  the string form by whether an operand names an XMM register. Three cases and
  the difference between the first two is the reason this cannot be a
  whole-register move: register-to-register preserves the upper lane, memory-to-
  register zeroes it.
* `translateSseMovHalf`, `translateSseCvtFlt`, `translateCvtTt2Si`,
  `translateSseUnpck`, `translateSseShufp`, `translateSseMovMsk` -- MOVLP\*/
  MOVHP\*, the float-to-float conversions, the truncating conversions to
  integer, UNPCK\*, SHUFP\* and MOVMSKP\*.

Plus the two orphans wired, and MOVUPS/MOVUPD pointed at the whole-register
move they always were.

### Two rounding bugs found while reading the neighbours

`translateCvtSd2Si` carried the comment *"Round toward nearest (C default); use
FPToSI (truncation)"* -- two halves of a sentence that contradict each other.
`FPToSI` truncates, so `cvtsd2si rax, xmm0` on 2.7 answered 2. The instruction
that truncates is `CVTTSD2SI`, which is why it is a separate opcode; both are
now what they say they are, and the test for each would fail on the other's
implementation.

`translateCvtPs2Dq` had the same shape: it truncated, `CVTPS2DQ` rounds, and
`CVTTPS2DQ` -- the one it was actually implementing -- was not in the table.

Both also took their destination width from `_basicMode`, so a 64-bit program
was assumed to be converting into a 64-bit register. The width is the operand's:
`cvttsd2si eax, xmm0` is a 32-bit conversion inside a 64-bit program. For
in-range values the two agree after `storeOp` truncates, which is why no test
here claims this as a behaviour fix -- it is written from the right source now
and that is all.

### The emulator returned zero for every `extractelement`

```cpp
void LlvmIrEmulator::visitExtractElementInst(llvm::ExtractElementInst& I)
{
	GenericValue dest;
	_globalEc.setValue(&I, dest);
}
```

Identical to the `visitExtractValueInst` defect recorded above, and reachable
by anything that reads a lane. `ADDPD` passed its test on the first run because
a whole-vector `fadd` never extracts; `ADDSD` answered 0.0 for 1.0 + 2.0. Thirty
of the new tests failed on it, which is how it was found, and no test could have
found it before because none of them had ever used a vector.

`visitInsertElementInst` is fixed alongside it: it sized the result from its
input vector, and an insert into `poison` arrives with an empty `AggregateVal`,
so the inserted lane was dropped.

### x86-64 in the multiarch corpus, as the control column

`build_multiarch_corpus.sh` built four architectures because the x86-64 corpus
already existed. But the parity question is "is every architecture at x86-64's
level", and that is only answerable if x86-64 is measured on the same sources,
the same flags and the same tools. It is built here now too, through its
triple-prefixed driver (`x86_64-linux-gnu-gcc`) so that nothing below needs a
special case, and ARCH-01 and COV-01 both report it alongside the other four.

Its first COV-01 number was the lowest of the five.

### COV-01 is a gate now, on all five

Measured over the 210-binary corpus, after everything above:

| arch    | rate   | what is left |
|---------|--------|--------------|
| x86_64  | 0.9931 | `HLT` -- one per binary, alignment padding |
| arm     | 1.0000 | nothing |
| arm64   | 0.9994 | `LD1`/`ST1` (NEON, 2 each) |
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |

x86-64 is the lowest of the five. What is left of it is `HLT`, once per binary,
in the alignment padding after `_start` -- code that never runs. It stays
uncovered rather than being pointed at a translator that emits the pseudo-asm
call it already emits, which would move the number and change nothing.

ARM's uncovered list no longer carries `<id 52> NO ENTRY`. That was
`ARM_INS_FCONSTD` -- `vmov.f64 d0, #1.0`, the VFP move-immediate, which
capstone prints as "vmov" but gives an id of its own, and which was not a key
in the table. `FCONSTS` is its single-precision twin and was equally absent.
`translateVfpMov` already handled the `ARM_OP_FP` operand these carry, so both
are one dispatch entry each. The rest of ARM's list took a change to the tool
before it could be read at all; that is the next section.

The floors are the measured values, not a notch below them. That needed one
correction to be possible at all: the rate is printed to four decimals and was
compared as a full double, so `6003/6045` displayed as `0.9931` and compared as
`0.99305`, and every architecture failed the floor copied from its own output.
A gate whose displayed number and compared number differ is a gate nobody can
set a floor for. It compares at the printed precision now, and the self-test
covers the three outcomes -- no floor, an unreachable floor, a floor of zero --
each of which flips if the gating is removed. Falsified by making the failure
branch `return 0`: two of the four self-test cases fail.


## What 32-bit ARM was really missing, once it could be measured

COV-01's ARM row came with a paragraph of apology: 536 bytes it could not
decode, and an uncovered list led by `STC`, `CDP` and `LDC` -- coprocessor
instructions that are not in these binaries at all. The tool's own header said
so: an ARM figure from it "is a lower bound with a wide error bar, and its
uncovered list should not be used to decide what to implement."

That is fixable, and the fix is in the ELF. 32-bit ARM objects carry `$a`, `$t`
and `$d` mapping symbols marking runs of ARM code, Thumb code and inline data.
Following them, COV-01 disassembles each run in the mode it is actually in and
drops the literal pools rather than pretending to decode them:

```
                 decoded   skipped   covered     rate   uncovered kinds
before              5300       536      5055   0.9538   24
after               8060         0      7698   0.9551    6  (3808 bytes of data)
```

The rate barely moved. Everything else did. `skipped` is zero, a third more
instructions are being read at all, and the uncovered list stopped being
fiction:

```
ARM_INS_HINT   238   2.95%   listed, null
ARM_INS_IT      78   0.97%   listed, null
ARM_INS_ADR     43   0.53%   listed, null
ARM_INS_VPUSH    1           listed, null
ARM_INS_VPOP     1           listed, null
ARM_INS_ORN      1           listed, null
```

Six real instructions instead of twenty-four phantoms, and three of them are
one line of dispatch each.

### `ARM_INS_NOP` is a key capstone does not produce

`HINT` is capstone's id for the entire hint space -- `nop`, `yield`, `wfe`,
`wfi`, `sev`, `sevl`, `dbg`, the pointer-authentication and branch-target
hints -- and `ARM_INS_NOP` exists but is not what a `nop` decodes to. The table
had `ARM_INS_NOP` pointing at `translateNop` and no entry for `HINT`, so the
translator that does the right thing sat on a key nothing arrives at. Third
time this branch has found that shape: `ARM_INS_VMRS` against `ARM_INS_FMSTAT`,
a duplicate `ARM64_INS_HINT` key, and now this.

There was a test. It was called `ARM_INS_NOP`, and it asserted that `nop` comes
out as a call to `__asm_nop`:

```cpp
EXPECT_JUST_VALUES_CALLED({
    {_module.getFunction("__asm_nop"), {}},
});
```

Both halves wrong and agreeing with each other. The test was written from the
behaviour observed rather than the behaviour the instruction has, so it locked
the defect in: a `nop` that produced nothing would have failed it.

Most of the hint space is genuinely a no-op and is translated as one. `WFI`,
`WFE`, `SEV`, `SEVL` and `DBG` are not -- they wait on or signal an external
event -- and keep their pseudo-asm call rather than being silently dropped,
which is the difference between "this does nothing" and "we do not model what
this does". Capstone gives these no operand at all, so the mnemonic is the only
thing that separates them.

### Thumb reads PC as address + 4, at every width

```cpp
return llvm::ConstantInt::get(
        getDefaultType(),
        ((i->address + (2*i->size)) >> 2) << 2);
```

ARM reads PC as the instruction's address plus 8, and `2*size` is 8 for a
4-byte ARM instruction. Thumb reads it as the address plus 4 -- and plus 4
whether the instruction is 16 or 32 bits wide. `2*size` is 4 for a 16-bit Thumb
instruction, which is right, and 8 for a 32-bit one, which is four bytes past
where the architecture says PC is. Every Thumb-2 PC-relative load and every
32-bit `ADR` was off by a word.

It looked correct because it is correct in two of the three cases, and the
third had no test. `addw r0, pc, #20` at address 0 answered 28 and answers 24
now.

### `IT` is a no-op, and that is a fact about the dispatcher

`IT` makes the next one to four instructions conditional. Capstone puts that
condition on each of those instructions' `cc`, and `arm.cpp`'s dispatcher
already wraps any instruction whose `cc` is not `AL` in a generated condition.
So the block header has no effect of its own and the right translation is
nothing -- but only because of what happens elsewhere, which is why the entry
carries a comment saying so rather than looking like an oversight.

### `ADR`, `ORN`, `VPUSH`, `VPOP`

`adr rN, label` is PC plus an immediate -- how a compiler names an address in
its own function without a literal pool. Capstone reports the offset, not the
resolved address, so a translation that stored the immediate would answer 20
where the answer is 24.

`orn rd, rn, op2` is `rn | ~op2`; the complement is on the second operand, not
on the result.

`VPUSH`/`VPOP` cannot reuse `translateLdmStm`, which writes every slot at
`getArchByteSize()` -- four. A D register is eight, so a list of them pushed
four bytes apart overlaps itself. The test pushes `{d0, d1}` and checks both
addresses, which is the assertion the integer translator would fail.

### Where ARM ends up

```
arch         decoded   skipped    covered     rate  uncovered-kinds
arm             8060         0       8060   1.0000  0  (3808 bytes mapped as data)
```

Every instruction in the 42 ARM binaries has a translator. The number it
started this branch at was 0.9517, and the number it would still be showing
without the mapping-symbol change is 0.9551 -- with six real gaps hidden behind
twenty-four that were never there.

### One failure that has not come back

One C2L-01 run in this sequence reported `PPC_INS_MR/CS_MODE_64` failed. It is
a three-line test -- set r11, `mr 0, 11`, expect r0 -- in an architecture this
branch had not touched since the floating-point work, and it has not failed
again: the same binary has since run the whole suite clean twelve times, five
of them back to back with nothing else on the machine, plus six runs of the
PowerPC tests alone.

It is recorded rather than explained. "Flake" is not a root cause and this is
not being called one; what can be said is that it did not reproduce, and that
the counts and the gate are green on the tree being pushed. If it returns, the
thing to look at first is `GenericValue`'s default constructor, which leaves
`DoubleVal` uninitialised -- the same shape as the `extractelement` and
`extractvalue` defects above, and the only source of nondeterminism this
emulator has.


## The hollow-binary check reported failure exactly when it succeeded

`--link static` has been unusable since the hollow check went in, and the
reason is four characters:

```sh
if ! "${triple}-nm" "${bin}" 2>/dev/null | grep -qE '^[0-9a-f]+ [Tt] main$'; then
```

`set -o pipefail` is on. `grep -q` exits the instant it matches, which closes
the pipe; `nm` is then killed by SIGPIPE and exits 141; `pipefail` makes 141 the
pipeline's status; the `!` turns that into "no defined main". The check reports
a hollow binary **because** the symbol is there.

It never fired on the dynamic corpus and fired on all 210 binaries of the
static one, which is the tell. A dynamically linked cross binary has a hundred
or so symbols -- a few kilobytes, well inside the pipe buffer, so `nm` finishes
writing before `grep` has decided anything and never sees SIGPIPE. A static one
has thousands, comfortably past 64 KB, so `nm` is always still writing.

`grep` without `-q` reads its input to the end. That is the fix.

The rest of the repository was checked for the same shape. Seven other scripts
combine `pipefail` with `| grep -q`, and all of them pipe from `printf` of a
small variable, `find` over a handful of files, or `head -n 8` -- outputs that
fit in the pipe buffer, where the producer completes regardless of when the
consumer leaves. The hazard is specifically a large producer, which is why the
one instance that mattered was the one reading a symbol table.

This one is mine, from earlier in this branch, and it is the same lesson as the
rest: the check was written and observed to pass on the corpus at hand, and the
corpus at hand could not make it fail.

## What a static corpus says, which is a different thing

`--link static` drags in all of glibc: its hand-written SIMD, its string
routines, its syscall stubs. It is deliberately not a gate -- it is a harder
measurement than the x86-64 corpus rather than the same one -- but it is the
best guide there is to what to implement next. Over the same 210 programs built
`-static`:

| arch    | decoded   | rate   | led by |
|---------|-----------|--------|--------|
| x86_64  | 5,230,330 | 0.9447 | `VPCMPEQB`, `VMOVDQU`, `PMOVMSKB`, `VPMOVMSKB`, `VMOVDQU64`, `TZCNT`, `PALIGNR`, `KMOVD` |
| arm64   | 3,592,964 | 0.9836 | `UDF`, `MRS`, `EXT`, `ST1B`, `SVC` |
| arm     | 3,398,752 | 0.9937 | `MRC`, `PLD`, `TBB`, `STCL`, `LDCL` |
| mips    | 4,292,791 | 0.9952 | `RDHWR`, `PREF` |
| powerpc | 4,640,553 | 0.9980 | `SC` |

The question this branch was given was whether the other four architectures
reach x86-64's level. On glibc-heavy code they are all **ahead** of it, and not
narrowly: x86-64 is four points behind the next worst, because its list is
AVX2 and AVX-512 -- `VPCMPEQB`, `VMOVDQU64`, `KMOVD`, `VZEROUPPER` -- and
nothing in `src/capstone2llvmir/x86` models a YMM or ZMM register at all.

That is recorded, not fixed. AVX needs 256-bit registers in the register file
before any of its instructions can be translated, which is a subsystem rather
than a table entry, and none of it is in user code -- it is in `memcpy` and
`strlen`, behind ifunc dispatch.

Four arm64 ids have **no entry at all**, which is the "the table predates the
instruction" shape rather than a decision: `UDF` (17,340 occurrences, the most
frequent untranslated arm64 instruction in this corpus), `LD1B` and `ST1B`
(SVE) and `LDG` (MTE). They already take the pseudo-asm path, since an id with
no entry falls through to it, so adding entries would move the number without
changing what the decompiler emits -- which is the same reason x86-64's `HLT`
is left where it is.

COV-01 follows AArch64's `$x`/`$d` mapping symbols now as well as ARM's
`$a`/`$t`/`$d`, so arm64's list is code rather than padding. It changed nothing
on the dynamic corpus, whose `.text` carries no inline data, and it is the
honest thing to do before quoting an arm64 uncovered list at all.


## Two aborts the new translations reached, and the gates that found them

ctest-linux 292, at `fb31221`, went red on CC-01 and on ARCH-01's arm64 floor.
Both were mine, both were aborts rather than wrong answers, and neither was
reachable before this branch because before this branch the instructions that
reach them came out as `__asm_*` calls.

This is the gates doing their job on the first run after the change, which is
what they are for. It is also the third time in this branch that widening what
gets translated has turned an unreachable path into a reachable one, after
`fneg` and `freeze`.

### `atomicrmw` was inlinable, and inlining it aborted

```
llvm_instruction_converter.cpp:554: visitInstruction: Fail (unsupported
  instruction: %4 = atomicrmw add ptr %2, i32 %3 acq_rel, align 4)
#9  InstVisitor<LLVMInstructionConverter,...>::visitAtomicRMWInst
#16 LLVMInstructionConverter::convertExtCastInstToExpression
#17 LLVMInstructionConverter::visitZExtInst
```

`BasicBlockConverter` converts `atomicrmw` as a *statement*, and has done since
the C11-atomics fix earlier in this branch. `LLVMInstructionConverter` converts
values used as operands into *expressions*, and has no case for it. Which of
the two sees an instruction is decided by `LLVMSupport::isInlinableInst()`, and
that said yes: one use, same basic block, not a call or a load or a phi.

ARM64's `ldadd` writes the old value of the memory into a register, so the
`atomicrmw` has exactly one use -- a `zext` -- and every binary containing one
ended the decompiler. `generated_atomic_counter-arm64-gcc-O0`, the whole
reason that source is in the corpus.

`atomicrmw` and `cmpxchg` belong on the not-inlinable list for the reason
`LoadInst` is already on it, and more so: each is a load *and* a store, so
moving one to its use site reorders a write. With them on it, the use becomes
the variable `BasicBlockConverter` already assigns to.

Three tests, in `tests/llvmir2hll/llvm/llvm_support_tests.cpp`. The third one
is there because the first two would pass against a function that always
returns false.

### `insertvalue` does not take a vector

```
Assertion `ExtractValueInst::getIndexedType(Agg->getType(), Idxs)
           == Val->getType() && "Inserted value must match indexed type!"'
#13 (anonymous namespace)::convertToType  ir_modifier.cpp:394
#15 IrModifier::modifyFunction
#16 ParamReturn::applyToIr
```

All fifteen CC-01 failures were this one assertion, and so were four of
ARCH-01's five.

`convertToType()` grouped vectors with structs and arrays and converted into
them with `insertvalue`. `extractvalue` and `insertvalue` are struct and array
only: `ExtractValueInst::getIndexedType(<2 x double>, {0})` returns **null**,
so the assertion compares null against `double` and fires. Checked directly
against LLVM 20 rather than inferred -- `getIndexedType` answers `<null>`, and
`insertvalue <2 x double> undef, double %a, 0` builds without complaint on an
assertions-off LLVM and is rejected by the verifier as "Invalid InsertValueInst
operands!". CI's LLVM has assertions on, so it aborts instead.

The vector forms are `insertelement` and `extractelement`. The other direction
was missing too and was worse: a `<2 x double>` asked to become a `double` fell
off the end of the chain into `llvm_unreachable()`.

`ParamReturn` asks for the conversion when it rewrites a function's return
type, which is why this arrived the moment `ADDSD` and friends started
producing `<2 x double>` values instead of pseudo-asm calls.

Falsified both ways against a real module: with the fix, `insertelement` and
`extractelement` and a clean verifier; with `ir_modifier.cpp` alone reverted,
`Invalid InsertValueInst operands!` on the first conversion and
`UNREACHABLE executed at ir_modifier.cpp:420` on the second.

### What ARCH-01's new column said

```
ARCH-01: x86_64    38/42   0.9048
ARCH-01: arm       42/42   1.0000
ARCH-01: arm64     41/42   0.9762
ARCH-01: mips      42/42   1.0000
ARCH-01: powerpc   42/42   1.0000
```

x86-64 is in this table for the first time, unfloored, as the control column --
and on its first run it was the worst of the five. That is the entire argument
for putting it there.


## Asking the question up front instead of four times after the fact

Four aborts of one shape were found in this branch, each by a corpus run after
the change that made it reachable:

| instruction | reached by | found |
|-------------|------------|-------|
| `fneg` | every FP negation, any architecture, since the initial import | the first corpus with a `float` in it |
| `freeze` | LLVM's own optimiser, on an integer program | widening ARCH-01 to the whole corpus |
| `atomicrmw` | ARM64 LSE, whose `ldadd` writes the old value to a register | ctest-linux 292 |
| `cmpxchg` | ARM64 `cas` | the same |

All four end in `LLVMInstructionConverter::visitInstruction()`, which calls
`FAIL()` and aborts the process, and all four are answerable from LLVM's own
headers with no corpus at all. `scripts/ci/check_ir2hll_opcodes.py` asks it.

An instruction reaches the expression converter unless `isInlinableInst()` or
`shouldBeConvertedAsInst()` keeps it out, so every opcode in LLVM's
`Instruction.def` has to be one of four things: it has a `visit<Class>` in the
converter; it is named in `isInlinableInst()`'s exclusion list; it is a
terminator, excluded by `isTerminator()`; or it is in the script's
`ACCOUNTED_ELSEWHERE` table with the reason it cannot arrive. There are six in
that last group -- `alloca` (excluded a layer up), `store` and `fence` (void
result), and the three exception-handling pads, which no translator emits and
no machine code carries.

Today, on LLVM 20, that is 43 + 7 + 11 + 6 = 67, the whole list.

Run against the tree that broke ctest-linux 292 it names `AtomicRMW` and
`AtomicCmpXchg`. Run against `894733d`, the initial import, it names all four:

```
IR2HLL-01: FAIL these opcodes reach visitInstruction(), which aborts:
           FNeg (UnaryOperator)
           AtomicCmpXchg (AtomicCmpXchgInst)
           AtomicRMW (AtomicRMWInst)
           Freeze (FreezeInst)
```

The self-test neuters one accounted-for opcode at a time -- deleting a
converter case, and dropping an entry from the exclusion list -- and requires a
failure for each, then requires the real tree to pass. Without that last part a
checker whose parse silently broke would report "OK" on everything.

It reads the LLVM the product is built with, not whatever the runner has
installed: `build/linux/external/src/llvm-project/llvm/include/llvm/IR/Instruction.def`
in CI, and whatever `llvm-config` points at locally. An LLVM upgrade that adds
an instruction is exactly the case this is for, and asking a different LLVM
than the one being compiled against would answer a different question.


## Where this lands

ctest-linux 293 at `811319d`, every step green:

```
ARCH-01: x86_64    42/42   1.0000
ARCH-01: arm       42/42   1.0000
ARCH-01: arm64     42/42   1.0000
ARCH-01: mips      42/42   1.0000
ARCH-01: powerpc   42/42   1.0000
ARCH-01: overall  210/210  1.0000
```

Same 42 sources, same flags, same tools, all five architectures. That is the
question this branch was given, and it is now a floor rather than a reading:
all five are at `--arch-min 1.0`.

COV-01 on the same corpus, also a floor:

| arch    | rate   | what is left |
|---------|--------|--------------|
| x86_64  | 0.9931 | `HLT` in alignment padding |
| arm     | 1.0000 | nothing |
| arm64   | 0.9994 | `LD1`/`ST1` (NEON, 2 each) |
| mips    | 1.0000 | nothing |
| powerpc | 1.0000 | nothing |

### Prefetch hints are not instructions to model

`PLD`, `PLDW` and `PLI` on ARM, `PRFM`/`PRFUM` on ARM64 and `PREF` on MIPS
touch no register and no byte of memory and raise no addressing exception.
There is nothing to translate, which is the answer `NOP` and `BTI` already
get, and as `nullptr` entries they came out as `__asm_pld` calls -- 2,142 of
them across the 42 static ARM binaries, 924 `__asm_prfm` on ARM64 and 840
`__asm_pref` on MIPS. Noise in the output for instructions that do nothing.

Falsified one architecture at a time by reverting the dispatch entries: each
time, exactly the new tests for that architecture fail.

C2L-01 floors: Arm 592 -> 596, Arm64 481 -> 482. 4,650 tests.


## The PowerPC flake was a silent miscompilation

`PPC_INS_MR/CS_MODE_64` failed once in a C2L-01 gate run and was recorded above
as "one failure that has not come back", with a note that the thing to look at
if it returned was `GenericValue`'s default constructor. It came back on the
next gate run, so it got looked at properly.

It reproduces. Not in isolation -- 120 runs of that one test under four
competing CPU hogs, all clean -- but in the PowerPC suite under the same load
it failed on run 14 of 25. The difference is what the process had allocated
before it, which is the shape of an uninitialised read.

The failing run's IR says what happened:

```
  4660 :   %0 = load i64, ptr @r11
  4660 :   store i64 %0, ptr @r0
     0 :   %1 = icmp slt i64 %0, 0
     0 :   store i1 %1, ptr @cr0_lt
     1 :   %2 = icmp sgt i64 %0, 0
     ...
```

`mr` is not a record form and sets no flags. `storeCr0()` runs only when
`pi->update_cr0` is set, so capstone had reported this `mr` as `mr.`.

### Where it comes from

```c
void PPC_post_printer(csh ud, cs_insn *insn, char *insn_asm, MCInst *mci)
{
	...
	if (strrchr(insn->mnemonic, '.') != NULL) {
		insn->detail->ppc.update_cr0 = true;
	}
}
```

The post-printer reads `insn->mnemonic`. `fill_insn()` calls it *before* it
copies the mnemonic in -- the line above the copy in capstone's own source is a
commented-out `// memset(mnem, 0, CS_MNEMONIC_SIZE);`. So the field it reads is
whatever was in the buffer beforehand.

With `cs_disasm()`'s array that is the previous instruction's mnemonic, which
is capstone's bug and is at least deterministic. `capstone2llvmir` does not use
that: it calls `cs_malloc()` per instruction so that every `cs_insn` can be
kept, and `cs_malloc()` sets only `detail` -- `mnemonic` is uninitialised heap.

Demonstrated directly, same four bytes, same handle, three buffers:

```
trial 0 (cleared):        mnem='mr' ops='r0, r11' update_cr0=0 bh=0
trial 1 ("addc." in it):  mnem='mr' ops='r0, r11' update_cr0=1 bh=0
trial 2 ("bdnzt+" in it): mnem='mr' ops='r0, r11' update_cr0=0 bh=1
```

So this was never a test problem. Any PowerPC instruction RetDec decodes can
be given a CR0 update it does not perform, decided by the allocator, and the
conditional branches that read `cr0_lt`/`cr0_gt`/`cr0_eq` downstream read it.
That is a wrong answer rather than a crash, which is the worse kind, and it
predates this branch entirely.

Capstone reads the mnemonic in exactly one architecture's post-printer --
PowerPC's, three times, for `'+'`, `'-'` and `'.'`. The other six read only the
`insn_asm` buffer they are handed.

### The fix, and the test

`cs_malloc()` is wrapped: the two strings capstone reads before it writes them
are cleared. It belongs in this repository rather than in the pinned capstone,
and it is two assignments.

The test primes the hazard rather than waiting for it. It assembles first, so
nothing else allocates in between, then frees a chunk of exactly
`sizeof(cs_insn)` carrying `"addc."` at the mnemonic offset; glibc's tcache is
LIFO per size class, so `cs_malloc()`'s next allocation of that size gets it
back. Falsified by removing the two assignments: the new test fails, and
nothing else does.

C2L-01 floor: Powerpc 880 -> 882. 4,652 tests.

### What the first write-up got wrong

Recording it as "did not reproduce, here is where to look if it returns" was
the wrong call. Twelve clean runs of the whole suite is not evidence of
absence when the failing condition is heap contents: the runs that reproduce
it are the ones under load, and I had been running it on an idle machine. The
second occurrence is what made the difference, and it should not have taken
one.


## IR2HLL-01 found one on its first CI run, and one in itself

ctest-linux 294 was green on everything except the gate added the commit
before, which is the gate doing its job: it reads the LLVM the product is
compiled against, LLVM 23.1.0, and the local run had read the system LLVM 20.

```
IR2HLL-01: FAIL these opcodes reach visitInstruction(), which aborts:
           FNeg (FPUnaryOperator)
           FAdd (FPBinaryOperator)
           FSub (FPBinaryOperator)
           FMul (FPBinaryOperator)
           FDiv (FPBinaryOperator)
           FRem (FPBinaryOperator)
           PtrToAddr (PtrToAddrInst)
```

Six of those seven are the checker's fault and one is real.

**The six.** LLVM 23 split `FNeg` out of `UnaryOperator` into `FPUnaryOperator`
and the five floating-point arithmetic opcodes out of `BinaryOperator` into
`FPBinaryOperator`. `InstVisitor` generates a visit method per class and each
one falls through to its base's:

```cpp
RetTy visitFPBinaryOperator(FPBinaryOperator &I) { DELEGATE(BinaryOperator); }
```

so a converter defining `visitBinaryOperator` covers `FPBinaryOperator` as
well, and the six were never uncovered. The checker was matching class names
without following that chain. It reads the chain out of `InstVisitor.h` now
rather than hard-coding it, because which classes exist and what they delegate
to is precisely what moves between LLVM releases.

**The one.** `ptrtoaddr` is new in LLVM 22: the address part of a pointer,
split out of `ptrtoint` for targets with fat pointers, where the capability
metadata is dropped. `InstVisitor` delegates it to `visitCastInst`, and this
converter names the specific casts rather than `CastInst`, so it reached
`visitInstruction()` and would have aborted -- the fifth instance of that
shape, after `fneg`, `freeze`, `atomicrmw` and `cmpxchg`, and the first one
found before a binary reached it rather than after.

There is nothing in BIR to drop, so `visitPtrToAddrInst` is the same cast as
`ptrtoint`, behind `#if LLVM_VERSION_MAJOR >= 22` because the class does not
exist in the LLVM the standalone suites build against.

### A local gate weaker than the CI one

The checker searched `llvm-config` first and the vendored build tree last, so
a local run answered for LLVM 20 while CI answered for LLVM 23 -- and LLVM 20
does not have `ptrtoaddr` at all. That is the same shape as the push gate that
ran `--self-test` where CI ran the real check, and as the workdir guard that
trusted an unstamped directory: a checker more permissive than the thing it
stands in for.

It looks in the vendored tree first now. Where there is no build tree, as on a
fresh checkout, it falls back and the line it prints names which LLVM it asked,
so the weaker answer is visible rather than assumed:

```
IR2HLL-01: 67 opcodes in /usr/lib/llvm-20/include
IR2HLL-01: 69 opcodes in build/linux/external/src/llvm-project/llvm/include
```

Falsified against both: removing `visitPtrToAddrInst` fails the LLVM 23 check
by name and leaves the LLVM 20 check passing, which is correct -- the opcode
is not in LLVM 20.


## The last two on ARM64: LD1 and ST1

COV-01's arm64 row had two entries left, `LD1` and `ST1`, two occurrences
each, and the note above said NEON. That is true of the family and not of
these two forms.

`ld1 {v0.16b}, [x0]` is a plain 128-bit load. `ld1 {v0.8b}, [x1]` is a 64-bit
one that zeroes the top half of the register, which is what every D-form write
does. A list moves consecutive registers to or from consecutive addresses.
None of that needs a lane model, and that is the whole reason these two can be
translated while the rest of NEON stays where it is: `V0`..`V31` are `i128`
globals in this translator, so the arrangement decides only the total width.
`ld1 {v0.4s}, [x0]` and `ld1 {v0.16b}, [x0]` move the same 128 bits.

Deliberately left on the pseudo-asm path: writeback forms (`[x0], #16` has to
update the base register), lane forms (`ld1 {v0.s}[2], [x0]`, which the width
helper rejects by returning zero), and LD2/LD3/LD4 and their stores, which
de-interleave rather than copy.

Four tests. The interesting two are the D-form pair: the load starts with
`0xdeadbeefdeadbeef` in the upper half of `V0` so that failing to zero it is
visible, and the list store checks that the second register lands eight bytes
on rather than at the same address or sixteen.

Falsified by forcing the access width to 128 bits and the list offset to zero.
The suite does not pass: it aborts, in `APInt::getZExtValue()`, because a
128-bit store then lands where the harness reads 64 bits. A blunt signal, but
an unambiguous one -- the mutation does not survive.

### Where COV-01 ends up

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64          6045         0       6003   0.9931  1
arm             8060         0       8060   1.0000  0  (3808 bytes mapped as data)
arm64           6742         0       6742   1.0000  0
mips            8519         5       8519   1.0000  0
powerpc         8904         0       8904   1.0000  0
```

Four of the five translate every instruction their 42 binaries contain. The
fifth is x86-64, and what it has left is `HLT` in the alignment padding after
`_start` -- code that never runs. It stays uncovered rather than being pointed
at a translator that emits the pseudo-asm call it already emits.

The branch started with x86-64 as the only architecture anything measured, and
ends with x86-64 as the only one below 1.0.


## x86-64 is the one below 1.0, and the static corpus says why

COV-01 over the 210-binary dynamic corpus has x86-64 at 0.9931 with one
uncovered kind. That is a true number about those binaries and a misleading
one about the translator, because a dynamically linked `hello world` contains
almost nothing: the interesting code lives in `libc.so.6`, which the corpus
binaries call rather than contain.

`scripts/build_multiarch_corpus.sh --link static` builds the same 210 sources
with glibc linked in. Five million instructions instead of thirty thousand,
and a very different answer:

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64       5230330         0    4940904   0.9447  84
arm                                         0.9943
arm64                                       0.9840
mips                                        0.9954
powerpc                                     0.9980
```

(The four non-x86 rows are the rates from the same static build; only the
x86-64 row is reproduced in full here because it is the one this section is
about. ARM's is a lower bound with a wide error bar for the reason COV-01's
own header gives.)

x86-64 is the worst of the five by a wide margin, and the 289,426 instructions
it does not translate are not exotica. They are what glibc's string, memory and
maths routines are made of.

### What the gap is made of

The top of the uncovered list, by occurrences over 5,230,330 decoded:

```
X86_INS_VPCMPEQB     27914  0.53%      X86_INS_PCMPGTB       8600  0.16%
X86_INS_VMOVDQU      24208  0.46%      X86_INS_VMOVDQA       8348  0.16%
X86_INS_PMOVMSKB     21102  0.40%      X86_INS_VPANDN        8274  0.16%
X86_INS_VPMOVMSKB    19946  0.38%      X86_INS_VMOVUPS       7266  0.14%
X86_INS_VMOVDQU64    15913  0.30%      X86_INS_PCMPISTRI     6393  0.12%
X86_INS_TZCNT        11662  0.22%      X86_INS_VPMINUB       5730  0.11%
X86_INS_PALIGNR      10740  0.21%      X86_INS_PREFETCHT0    5544  0.11%
X86_INS_KMOVD        10519  0.20%      X86_INS_PUNPCKLQDQ    5004  0.10%
X86_INS_VPADDB        9890  0.19%      X86_INS_SYSCALL       4627  0.09%
X86_INS_VZEROUPPER    9336  0.18%      X86_INS_MOVNTPS       3360  0.06%
```

Roughly two thirds of it is AVX and AVX-512 -- every `V`-prefixed entry, plus
the `K` mask-register instructions. Those are not a table-entry problem. YMM0
and ZMM0 do not exist in this translator's register file at all, so there is
nothing for a `VMOVDQU` translator to write to; adding them is a subsystem,
with its own aliasing rules against XMM, and it is recorded here rather than
attempted.

The other third is SSE2 and BMI1 -- 128-bit and general-purpose, on registers
this translator already has. That part is a table-entry problem, and this
commit is the first batch of it.

### Batch A

| instruction | occurrences | now |
| --- | --- | --- |
| `PMOVMSKB` | 21,102 | `translateSseMovMsk` |
| `TZCNT` | 11,662 | `translateBitCount` |
| `PREFETCHT0` | 5,544 | `translateNop` |
| `MOVNTPS` | 3,360 | `translateSseMovWhole` |
| `PREFETCHT1` | 2,688 | `translateNop` |
| `MOVNTDQ` | 1,008 | `translateSseMovWhole` |
| `LZCNT`, `POPCNT` | below the cut | `translateBitCount` |
| `PREFETCH`, `PREFETCHNTA`, `PREFETCHT2`, `PREFETCHW` | below the cut | `translateNop` |
| `MOVNTDQA`, `MOVNTPD` | below the cut | `translateSseMovWhole` |
| `MOVNTI`, `MOVNTQ` | below the cut | `translateMov` |

**`PMOVMSKB`** is the single most frequent untranslated x86 instruction in the
corpus, and it is frequent for one reason: it is the second half of glibc's
SSE2 string routines. `PCMPEQB` compares sixteen bytes at a time, `PMOVMSKB`
asks which of those sixteen matched, and `TEST`/`Jcc` branch on the answer.
`PCMPEQB` was already translated, so the comparison was being computed and
then discarded at an `__asm_pmovmskb` call, and the branch after it read a
value from nowhere. It is the same sign-bit gather as `MOVMSKPS`, over sixteen
byte lanes instead of four dword ones, so it is four lines in the translator
that was already there.

**`TZCNT` and `LZCNT` are not `BSF` and `BSR`.** They share an encoding but
for the `F3` prefix, and on a CPU without BMI1 a `TZCNT` decodes and executes
as `BSF` -- which is the whole reason the encoding was chosen, and also the
whole reason translating one as the other is wrong. For a **zero source** they
disagree completely:

```
        src == 0            BSF                  TZCNT
        destination         unmodified           operand width (32 or 64)
        reported through    ZF = 1               CF = 1
```

A compiler emits `TZCNT` precisely so that it does not have to branch around
the zero case. `llvm.cttz`/`llvm.ctlz` with `is_zero_poison=false` have exactly
the defined behaviour, and that is why the second argument is `false` here and
`true` in `translateBsf`.

`POPCNT` is the odd one of the three on flags: `ZF` from the **source**, and
`CF`, `OF`, `SF`, `AF` and `PF` architecturally **cleared** rather than
undefined, so they are written. `TZCNT` and `LZCNT` leave those four alone,
because "undefined" is not a value worth asserting.

**The prefetches** are the fifth instance in this branch of the same shape,
after ARM's `PLD`/`PLDW`/`PLI`, ARM64's `PRFM`/`PRFUM` and MIPS's `PREF`: an
instruction that names an address, does not read it, writes nothing and cannot
fault. As `nullptr` entries they came out as `__asm_prefetcht0` calls with a
memory operand, which reads as a side effect the instruction does not have.

**The non-temporal moves** differ from their ordinary counterparts only in
cache allocation policy, which has no architecturally visible effect on the
value moved. `MOVNTPS`, `MOVNTPD`, `MOVNTDQ` and `MOVNTDQA` are the 128-bit
move `translateSseMovWhole` already is; `MOVNTI` and `MOVNTQ` are the plain
store `translateMov` already is.

### Deliberately not in Batch A

`SYSCALL`, 4,627 occurrences, stays on the pseudo-asm path and should. It is
an opaque, side-effecting transfer to the kernel that clobbers `RAX`, `RCX`
and `R11` and can do anything at all to memory; `__asm_syscall()` is a more
honest model of that than any sequence of loads and stores would be. Same for
`HLT` and `PCMPISTRI`.

`MOVNTSS` and `MOVNTSD` (AMD SSE4a) are left alone: zero occurrences, and the
only way to reach them shares a width calculation with `MOVD`/`MOVQ`, so the
change would perturb two translated instructions to reach two untranslated
ones that nothing emits.

### What the falsification could and could not show

Four mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| `TZCNT`/`LZCNT` dispatched to `translateBsf` | the five zero-source and count tests fail |
| `PMOVMSKB` treated as `MOVMSKPS` (four 32-bit lanes) | both `PMOVMSKB` tests fail |
| prefetch and non-temporal entries back to `nullptr` | those six tests fail |
| `POPCNT` no longer clearing the five flags | the three `POPCNT` tests fail |

One mutation did **not** fail, and it is worth recording rather than quietly
dropping. Flipping `is_zero_poison` from `false` to `true` -- the difference
between "the answer for a zero input is the width" and "the answer for a zero
input is poison" -- leaves the whole suite green, because
`tests/llvmir-emul` implements `llvm.cttz` as `APInt::countr_zero()` and
ignores the second argument entirely:

```cpp
else if (id == Intrinsic::cttz)
    dest.IntVal = APInt(bw, src.IntVal.countr_zero());
```

So that argument is correct on the instruction manual's authority and not on
this suite's, and the honest thing is to say so. It matters against a real
LLVM -- `is_zero_poison=true` tells the optimiser the source is never zero,
which for `TZCNT` is a lie and would license folding away the `CF` computation
that the zero case exists to report -- and nothing here can demonstrate it.
A test that cannot fail proves nothing; a test that cannot be written should
be admitted to, not implied.

### Where it leaves the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before          5230330         0    4940904   0.9447  84
after           5230330         0    4986268   0.9533  78
```

45,364 instructions, 0.86 points, six kinds. The measurement is in CI from
this commit -- `COV-01 instruction coverage (static corpus, x86-64)` -- and
unfloored on its first run for the reason the step's own comment gives: the
rate depends on which glibc the runner's cross toolchains link in, and a floor
measured here and asserted about there is the mistake that let the local
IR2HLL-01 gate read LLVM 20 while CI read LLVM 23. It is floored next commit
at whatever number CI itself reports.

### 182 instruction ids are not in the x86 table at all

While measuring this, COV-01 reported five uncovered kinds as `NO ENTRY`
rather than `listed, null`: `INCSSPQ` (630), `RDSSPQ` (378), `VPTERNLOGD`
(3,550), `VPTESTMB` (2,981) and `VPTESTNMB` (717). They are not in
`x86_init.cpp`'s table under any spelling. Checking the whole enum: of the
1,523 instruction ids in the pinned Capstone 5.0.9, **182 have no row**.

This is not a defect. The dispatch is

```cpp
auto fIt = _i2fm.find(i->id);
if (fIt != _i2fm.end() && fIt->second != nullptr)
```

so a missing id and a `nullptr` row behave identically -- both fall through to
the pseudo-asm path. It is a staleness signal: the table was written against
an older Capstone and instructions added since (CET shadow stack, AVX-512
VBMI, GFNI, the `K*` mask ops) were never listed. Recorded because the *next*
time someone reads "listed, null" as "everything is accounted for", these 182
are the counterexample.


## Batch B: the packed integer instructions glibc actually uses

After Batch A the static-corpus x86-64 rate is 0.9533 with 78 uncovered kinds,
and the top of what is left is still recognisable: `PALIGNR` 10,740,
`PCMPGTB` 8,600, `PUNPCKLQDQ` 5,004, `PMINUB` 3,168. All 128-bit, all on
registers this translator already has.

| instruction | occurrences | now |
| --- | --- | --- |
| `PALIGNR` | 10,740 | `translateSsePalignr` |
| `PCMPGTB`/`W`/`D`/`Q` | 8,600+ | `translateSsePcmpeq` |
| `PUNPCKLQDQ`, `PUNPCKHQDQ`, `PUNPCKLWD`, `PUNPCKH{BW,WD,DQ}` | 5,634+ | `translateSsePunpckl` |
| `PMIN`/`PMAX`, signed and unsigned, B/W/D | 3,546+ | `translateSsePminMax` |
| `PCMPEQQ` | — | `translateSsePcmpeq` |
| `PAVGB`, `PAVGW` | — | `translateSsePavg` |

Every one of these is decided by a property the mnemonic spells out and the IR
does not, and each of the three properties had a way of going wrong that was
already sitting in this file:

**Signedness.** `PCMPGT` is a **signed** comparison and `PMINU`/`PMAXU` are
unsigned ones. The two readings disagree on every lane whose top bit is set,
which in the code these appear in -- the output of a `PCMPEQB`, where a match
is `0xff` -- is all the interesting ones. So the tests use `0xff` against
`0x01` and `0x80` against `0x0f`, not values that happen to agree.

**Lane width.** `translateSsePcmpeq` ended its width switch with
`default: bits = 32`. That was harmless while exactly three ids reached it and
would have silently compared 32-bit lanes the moment `PCMPEQQ` -- sitting
`nullptr` two lines away in the table -- was pointed at it. The switch is total
now and throws on an id it does not know.

`translateSsePunpckl` had the same shape in a shorter spelling:

```cpp
unsigned bits = (i->id == X86_INS_PUNPCKLBW) ? 8 : 32;
```

correct for the two ids that reached it, `PUNPCKLBW` and `PUNPCKLDQ`, and
wrong for `PUNPCKLWD`, which was `nullptr` directly below them in the table.
Wiring it would have interleaved 32-bit lanes under a 16-bit mnemonic: no
crash, no assertion, a different answer. This is the same defect as the ARM
`ARM_INS_NOP`/`ARM_INS_HINT` and PowerPC `update_cr0` findings earlier in this
branch -- a table whose correctness depends on which entries happen to be
`nullptr`.

**Which half.** `PUNPCKH` takes the top half of each operand rather than the
bottom. Same shuffle, source indices moved up by half the lane count.

`PALIGNR` is the one with an arithmetic trap rather than a naming one. It
concatenates destination and source into 256 bits and takes a 128-bit window
`imm` bytes up. Written as one i256 shift it reads better; written as i128
operations it stays inside what `tests/llvmir-emul` executes, and the three
cases have to be separated in C++ anyway, because `imm == 0` and `imm == 16`
are exactly the two values that would make an LLVM shift equal to its
operand's width, which is poison. `imm >= 32` is architecturally zero.

`PAVG` rounds half up and adds **one bit wider than the lane**. At the lane's
own width `0xff + 0x02 + 1` wraps to `0x02` and the answer comes out `0x01`
instead of `0x81`. The widening is not a precaution, it is the instruction, so
the test uses inputs whose sum carries out of the lane.

### The emulator change that turned out to be dead

`visitBinaryOperator()`'s vector branch lists add, sub, mul, the four division
and remainder forms, and, or and xor -- and not `shl`, `lshr` or `ashr`, which
would fall into its `default:` and `llvm_unreachable()`. `PAVG` emits a vector
`lshr`, so the three cases were added.

The falsification run said they were not needed: removing them again left the
suite green. `InstVisitor` dispatches `Shl`, `LShr` and `AShr` to
`visitShl()`, `visitLShr()` and `visitAShr()`, each of which has its own
vector branch, so `visitBinaryOperator()` never sees them and the missing
cases are unreachable.

The cases were reverted and a comment left in their place saying why they are
absent. The four tests stay: the vector branches of the three shift visitors
had no test either way, and the eight packed-shift instructions
`PSLLW`..`PSRLQ` will depend on them.

This is the second time in two commits that a falsification run came back
green and the right reading was "check the mutation" rather than "report the
result" -- the first being the `PMOVMSKB` sed that matched the wrong
indentation. The difference is that this one was the code being wrong, not the
mutation.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| `PCMPGT` compares unsigned | 2 tests fail |
| `PCMPEQQ` at 32-bit lanes | 1 test fails |
| `PUNPCKLWD` at 32-bit lanes | 1 test fails |
| `PUNPCKH` takes the low half | 2 tests fail |
| `PMIN`/`PMAX` signedness flipped | 2 tests fail |
| `PALIGNR` shifts by bits, not bytes | 1 test fails |
| `PAVG` adds at the lane's own width | 2 tests fail |
| emulator vector shift cases removed | **suite stays green** -- see above |

### Where it leaves the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before Batch A  5230330         0    4940904   0.9447  84
after Batch A   5230330         0    4986268   0.9533  78
after Batch B   5230330         0    5015166   0.9589  68
```

74,262 instructions and sixteen kinds across the two. What is left above 0.01%
is, with three exceptions, AVX and AVX-512: `VPCMPEQB`, `VMOVDQU`,
`VPMOVMSKB`, `VMOVDQU64`, `KMOVD`, `VPADDB`, `VZEROUPPER` and the rest of the
`V`- and `K`-prefixed list, which need YMM and ZMM in the register file.

The three exceptions are deliberate, not pending: `PCMPISTRI` (6,393),
`SYSCALL` (4,627) and `HLT` (294). The first is a string comparison whose
result is an index computed from an aggregate of sixteen lane comparisons
under a four-way mode immediate -- translatable, but a subsystem rather than a
table entry, and modelling it wrongly is worse than not modelling it. The
other two are opaque by nature.


## Batch C: what is left of the gap that is not AVX

After Batch B the static corpus reads 0.9589 with 68 uncovered kinds. Sorted
by occurrences, everything above 0.01% is `V`- or `K`-prefixed except three
deliberate exceptions -- and a tail of general-purpose instructions that is
short enough to finish.

| instruction | occurrences | now |
| --- | --- | --- |
| `BLSMSK` | 1,008 | `translateBls` |
| `SHRX` | 807 | `translateShiftX` |
| `SARX` | 756 | `translateShiftX` |
| `MOVBE` | 672 | `translateMovbe` |
| `BZHI` | 633 | `translateBzhi` |
| `SHLX` | 381 | `translateShiftX` |
| `PAUSE` | 168 | `translateNop` |
| `WAIT` | 126 | `translateNop` |
| `ANDN` | 84 | `translateAndn` |
| `BLSR` | 84 | `translateBls` |
| `BEXTR`, `BLSI`, `RORX` | below the cut | as above |

**`ANDN` complements the first source, not the second.** `dst = ~src1 & src2`.
Two files over, `PANDN` is `~dst & src`. Both spellings look right and they
disagree on every input pair that is not symmetric.

**`BLS*`'s carry flag does not mean the same thing three times.** `BLSI` sets
`CF` when the source is **not** zero; `BLSMSK` and `BLSR` set it when the
source **is**. There is exactly one input -- zero -- on which the three
disagree, so the tests use it.

For `BLSMSK` the manual clears `ZF` outright rather than computing it, on the
argument that the result cannot be zero (all ones for a zero source, at least
1 otherwise). Computing it from the result gives the same answer for every
input and does not depend on that argument staying true.

**`BEXTR` has two shift amounts that come from a register**, and either can
exceed the operand width, where an LLVM shift is poison rather than zero. Both
are guarded by a select: past the top the extracted value is zero, and a
length at or beyond the width is the full mask -- which `(1 << len) - 1`
cannot produce without overflowing. Clamping the amount instead would answer
for a different instruction.

**`BZHI`'s `CF` is not `BLSR`'s question.** It reports that the index was at or
past the operand width, in which case nothing is zeroed.

**The BMI2 shifts do not touch the flags at all**, and that is the entire
reason a compiler emits them: the shift can be scheduled across a comparison.
Writing `SF`/`ZF`/`CF` in `translateShiftX` would be a wrong answer of the kind
that shows up several instructions later, at the `Jcc` that reads a flag this
instruction was chosen for not disturbing. The test for it is
`EXPECT_JUST_REGISTERS_STORED({{X86_REG_EAX, ...}})` -- it fails if anything
else was written.

### The second thing the emulator cannot show

`translateShiftX` masks the count to the operand width, because the hardware
does and because `shl i32 %x, 36` is poison. Removing that mask leaves the
suite green.

```cpp
unsigned getShiftAmount(uint64_t orgShiftAmount, llvm::APInt valueToShift)
{
    unsigned valueWidth = valueToShift.getBitWidth();
    if (orgShiftAmount < static_cast<uint64_t>(valueWidth)) return orgShiftAmount;
    // according to the llvm documentation, if orgShiftAmount > valueWidth,
    // the result is undfeined. but we do shift by this rule:
    return (NextPowerOf2(valueWidth-1) - 1) & orgShiftAmount;
}
```

For a 32-bit value that is `& 31` -- the same mask x86 applies -- so the
emulator masks whether the translator did or not. The mask stays, because what
ships is the IR and not the emulator's reading of it; the test stays, because
it pins the architectural answer; and the test's comment says it cannot
falsify the mask, which is the part that would otherwise be quietly implied.

Second instance after `llvm.cttz`'s `is_zero_poison`. Both are the emulator
being *more forgiving* than LLVM, which is the safe direction for a test
harness to be wrong in and the useless one for falsification.

### Falsification

Thirteen mutations, each reverted alone, each rebuilt and run. Twelve fail:

| mutation | result |
| --- | --- |
| `ANDN` complements the wrong source | 2 tests fail |
| `BLSI`'s `CF` direction flipped | 2 tests fail |
| `BLSMSK` uses `AND` rather than `XOR` | 1 test fails |
| `BEXTR`'s start and len swapped | 3 tests fail |
| `BEXTR`'s start guard removed | 1 test fails |
| `BEXTR`'s length guard removed | 1 test fails |
| `BZHI`'s `CF` direction flipped | 2 tests fail |
| `SARX` shifts in zeroes | 1 test fails |
| the BMI2 shifts write flags | 6 tests fail |
| `RORX` shifts instead of rotating | 1 test fails |
| `MOVBE` does not swap | 1 test fails |
| `PAUSE`/`WAIT` back to `nullptr` | 1 test fails |
| the shift count is not masked | **suite stays green** -- see above |

### Deliberately not in Batch C

`MULX`, `PDEP` and `PEXT` have zero occurrences in the corpus. `MULX` takes
`EDX`/`RDX` as an implicit operand, and guessing how Capstone reports an
implicit operand rather than checking is how a silent miscompilation starts;
`PDEP` and `PEXT` are bit-by-bit loops. `BLSIC` and the rest of AMD's TBM have
zero occurrences and no compiler that emits them.

### Where the three batches leave the static number

```
                 decoded   skipped    covered     rate  uncovered-kinds
before Batch A  5230330         0    4940904   0.9447  84
after Batch A   5230330         0    4986268   0.9533  78
after Batch B   5230330         0    5015166   0.9589  68
after Batch C   5230330         0    5019885   0.9598  58
```

78,981 instructions and twenty-six kinds. What is left is AVX, AVX-512, and
`PCMPISTRI`, `SYSCALL` and `HLT`, which are recorded above as deliberate.

### The local and CI numbers are not measured over the same bytes

ctest-linux 298 ran the new static step and reported

```
arch         decoded   skipped    covered     rate  uncovered-kinds
x86_64       5230246         0    4986184   0.9533  78
```

against 5,230,330 decoded here: 84 instructions apart, because the runner's
glibc is not byte-identical to this container's. The rate agrees to four
decimal places and the uncovered-kind count agrees exactly, which is the
evidence that made it safe to floor -- and the 84-instruction difference is
the evidence that made it right to measure in CI first rather than assert a
locally-measured floor about a machine that was never asked.


## COV-01 was counting 17,340 zero words as ARM64 instructions

Measuring the four non-x86 architectures over the static corpus put arm64 at
0.9840 -- the worst of the four -- and the single largest entry in its
uncovered list was an id the dispatch table does not list at all:

```
<id 1155>                    17340   0.48%  NO ENTRY
```

Capstone says that id is `udf`. AArch64's `udf` is the encoding whose top
sixteen bits are zero, so any all-zero word decodes as one.

GNU objdump finds no `udf` in these binaries and no all-zero word in `.text`
of the one I first looked at. Scanning all forty-two: seventeen of them have
exactly 1,020 all-zero words each, and in `bubblesort-arm64-gcc-O0` they are
one contiguous run of 4,080 bytes at `0x40f378`, immediately after
`_nl_cleanup_ctype`, covered by no `FUNC` symbol -- and inside an `$x` region,
so glibc has marked 4 KB of zero padding as **code**.

COV-01's data filter is the `$d` mapping symbols, and that is the right
mechanism when the producer marks its data. Here the producer did not.

### The fix, and why it is not the fix that moves the number

The answer is the same one x86-64's `HLT`-in-alignment-padding got: a rate is
only worth having if its denominator is instructions the program can execute,
so stop counting the padding, rather than pointing a translator at it.

`split_on_zero_runs()` splits each code region around runs of 64 or more zero
bytes. The threshold is sixteen AArch64 instructions or thirty-two x86
`add byte ptr [rax], al`, and no compiler emits either as a reachable
sequence. The excluded bytes are reported in **their own column**, not folded
into `skipped`:

```
arch         decoded   skipped     zeros    covered     rate  uncovered-kinds
x86_64       5230330         0         0    5019885   0.9598  58
arm          3398752         0         0    3379355   0.9943  20
arm64        3575624        42     69360    3542591   0.9908  47
mips         4292791        89         0    4273234   0.9954   3
powerpc      4640553      9561         0    4631149   0.9980  23
```

"Capstone could not decode this" and "this was not code" are different facts,
and a rule that quietly makes a rate better is the shape this branch has been
finding all along. Separate column, eight self-test cases pinning the
boundary (64 bytes dropped, 63 kept, leading, trailing, absolute offsets), and
the rule changes nothing on the **gated** dynamic corpus -- all five read
`zeros 0` there, so the existing CI floors are untouched.

Falsified by disabling the split: arm64 reads 0.9860 instead of 0.9908, 48
uncovered kinds instead of 47, and `udf` returns to the top of the list with
17,340 occurrences.

## ARM64: EXT, the bitwise selects, and the lane compares

With the padding out of the denominator, arm64's uncovered list is real:

| instruction | occurrences | now |
| --- | --- | --- |
| `EXT` | 5,124 | `translateNeonExt` |
| `CMEQ` | 1,512 | `translateNeonCmp` |
| `CMHS` | 294 | `translateNeonCmp` |
| `BIT` | 210 | `translateNeonBitSel` |
| `CMGE` | 42 | `translateNeonCmp` |
| `CMGT`, `CMHI`, `CMTST`, `BSL`, `BIF` | below the cut | as above |

**`EXT` is `PALIGNR` with the operands the other way round.** x86 makes the
destination the high half of the concatenation; AArch64 makes the second
source the high half. A translation copied across from the one written three
commits ago answers with the halves swapped for every index except zero, which
is why the test checks that byte 0 of the result is byte 4 of `vn` and byte 15
is byte 3 of `vm`.

It is done at the arrangement's own width rather than always at 128 bits: for
`.8b` the operation is 64-bit and the write zeroes bits 127:64, which is what
every D-form write does. The shifts are split by case in C++ for the same
reason `PALIGNR`'s are -- index zero would otherwise be a shift by exactly the
operand width. `index >= width` is architecturally UNDEFINED rather than zero,
so it goes to the pseudo-asm path instead of being given an answer.

**The three bitwise selects differ only in which register is the selector**,
and they are the one family of NEON data-processing instructions that needs no
lane model at all:

```
BSL vd, vn, vm    vd = (vn & vd) | (vm & ~vd)   the destination selects
BIT vd, vn, vm    vd = (vd & ~vm) | (vn & vm)   the second source masks
BIF vd, vn, vm    vd = (vd & vm) | (vn & ~vm)   the same, inverted
```

The three tests use identical operands and differ only in the mnemonic, so
each one is pinned by an answer the other two do not produce.

**The lane compares are one letter apart and half of them are signed.** `CMGE`
and `CMGT` are signed; `CMHS` and `CMHI` are the unsigned pair. The two
readings disagree on every lane with its top bit set, which for the byte lanes
a NEON string routine works on is all the interesting ones -- so the `CMHS`
and `CMGE` tests use identical operands (`0xff` against `0x01`) and expect
different answers. `CMTST` is not a comparison at all: `(vn & vm) != 0` per
lane.

`cmeq vd.<T>, vn.<T>, #0` -- compare against an immediate zero -- is the form
that matters, because it is how a NEON string routine asks which of sixteen
bytes is the terminator.

### Falsification

Eight mutations, each reverted alone, each rebuilt and run:

| mutation | result |
| --- | --- |
| COV-01's zero-run split disabled | arm64 reads 0.9860, `udf` leads the list |
| `EXT`'s two sources swapped | 3 tests fail |
| `EXT` always 128-bit | 1 test fails |
| `BIT` selects with the destination | 1 test fails |
| `CMHS`/`CMGE` signedness flipped | 3 tests fail |
| the compare's lane width forced to bytes | 1 test fails |
| `CMTST` made a comparison | 1 test fails |
| the ten dispatch entries back to `nullptr` | the suite aborts |

One mutation did not apply on its first attempt -- a multi-line Python string
inside a shell heredoc -- and the driver said so and skipped the run rather
than reporting a green suite. That guard exists because of the `PMOVMSKB`
mutation two commits ago that silently did not apply and was reported as a
result.

### What is left on ARM64, and why

`MRS`, 12,400 occurrences, is now the largest entry. 275 of the 293 in each
binary are `mrs xN, tpidr_el0` -- the TLS thread pointer -- and the rest are
`fpsr`, `fpcr`, `dczid_el0`, `ctr_el0` and `midr_el1`. Modelling `TPIDR_EL0`
as a register would be a real improvement: every TLS access currently ends at
an opaque `__asm_mrs()` call, and a modelled register would let the rest of
the pipeline propagate it. It is not done here because Capstone has no
`ARM64_REG_TPIDR_EL0` -- system registers are a separate namespace reported as
`ARM64_OP_SYS` -- so it needs a synthetic register id, and
`src/bin2llvmir/providers/abi/arm64.cpp` sizes its arrays with
`_id2regs.resize(ARM64_REG_ENDING, nullptr)`. An id past that end is an
out-of-bounds write, not a missing entry. That is a two-module change with a
memory-safety edge, and it is written down here rather than attempted between
two NEON batches.

`SVC` (4,206) and `BRK` (339) are opaque by nature, like x86's `SYSCALL`.

`ST1B` (4,620), `LD1B` (2,688), `WHILELO` (588), `PTRUE` (168), `CNTB` and
`CNTD` are **SVE**, and `LDG` (1,302), `ST2G`, `STZG`, `STZ2G`, `IRG` and
`GMI` are **MTE**. Both need registers that do not exist in this translator --
Z and P for SVE, an allocation-tag model for MTE. Recorded, not attempted, for
the same reason AVX is: a subsystem, not a table entry.

`UMAXP` (882), `SHRN` (798), `UMINP`, `ADDP`, `ADDV`, `UZP1`, `SADDL`,
`UADDW`, `XTN`, `SHL`, `UMOV`, `CNT` and `MVNI` are ordinary NEON lane
operations that this register model can express. They are a next batch, not a
limitation.

### A weakness in COV-01 itself, recorded

COV-01 counts an instruction as covered when the dispatch table has a function
pointer for it. On ARM64 that is not the same as translating it:

```cpp
if (ifVectorGeneratePseudo(i, ai, irb))
{
    return;
}
```

`translateMovi` and several others begin with that line, so for a vector
operand they emit exactly the `__asm_movi` call a `nullptr` entry would have
emitted -- and COV-01 scores them covered. The number it reports is therefore
an upper bound on ARM64, and the honest metric would count `__asm_*` calls in
the decompiler's **output** rather than function pointers in its table.
ARCH-01 already runs the decompiler over this corpus, so the measurement is
available; it is the next thing to build, and until it exists the ARM64 rate
should be read as "has a function for", not "translates".
