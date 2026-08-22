# Goto optimizer baseline (Q4)

Measurement only. **SAILR is not ported.** Do not treat this pass as
structure recovery.

Sources (read these; do not invent APIs):

- `include/retdec/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.h`
- `src/llvmir2hll/optimizer/optimizers/goto_cfg_optimizer.cpp`
- `include/retdec/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer.h`
- `src/llvmir2hll/optimizer/optimizers/goto_stmt_optimizer.cpp`
- `src/llvmir2hll/optimizer/optimizer_manager.cpp`
- `src/llvmir2hll/hll/hll_writers/c_hll_writer.cpp` (`CHLLWriter::visit(GotoStmt)`)

## What the pass does (from the code)

Two HLL-IR passes run on each function. Neither is SAILR.

### `GotoCFGOptimizer` (`getId()` = `"GotoCFG"`)

`GotoCFGOptimizer::runOnFunction` calls the anonymous `onePass()` up to 32
times until a pass makes no change.

`onePass()` walks successor-linked statement lists and applies:

| Pattern | Code (live `onePass`) | Rewrite |
|---------|------------------------|---------|
| **A** | `IfStmt` with no else / else-if, body a single `GotoStmt` (leading `EmptyStmt` skipped), target forward-reachable within 256 successors | Invert condition; body becomes the cloned intervening statements (`if (!cond) { … }`) |
| **B** | `GotoStmt` whose target is the immediate successor, and the goto is not itself a goto-target | Remove the goto |
| **D** | `GotoStmt` whose target equals the enclosing loop’s successor (passed as `loopExit` into `WhileLoopStmt` / `ForLoopStmt` bodies) | Replace with `BreakStmt` |

`collectBetween` / `isForwardReachable` cap walks at 256 statements.

**Pattern C is not applied.** The header describes a tail-goto → continue /
`while (true)` rewrite. The unused `FuncRewriter::tryPatternC` in the same
`.cpp` returns `false` (“let existing loop optimizers handle this”). The live
walker never implements C.

The header’s Pattern D prose (“goto to early return”) does not match the live
code, which only maps **loop-exit** gotos to `break`. Early-return collapsing
is the older `GotoStmtOptimizer` (below).

A first `FuncRewriter` class in the same file is unused; `runOnFunction` only
calls `onePass`.

### `GotoStmtOptimizer` (`getId()` = `"GotoStmt"`)

Visitor on `GotoStmt`: if the target statement is itself a `GotoStmt`,
`ReturnStmt`, `BreakStmt`, or `ContinueStmt`, clone that statement in place of
the goto and drop the label if nothing else targets it.

### Where they run

`OptimizerManager` runs `GotoCFGOptimizer` then `GotoStmtOptimizer` twice:
once with the early HLL-independent group, and again after var-def / empty-stmt
cleanup (`optimizer_manager.cpp`).

### How C is emitted

`CHLLWriter::visit(GotoStmt)` writes a comment
`decompiler: unstructured control flow` then `goto <label>;`.

## SAILR is not ported

The `goto_cfg_optimizer.h` comment says the pass “follow[s] the approach of
the SAILR algorithm (USENIX Security 2024).” That is a citation in a comment.

- No SAILR / angr structuring dependency.
- No schema-independent-region / goto-free guarantee.
- `docs/internal/retypd_sailr_llvm.md` Step 31 still lists SAILR as a
  3–6 month item blocked on LLVM alignment.
- This baseline measures the **existing** pattern rewriter only.

## How to count gotos in decompiled C

Count the token the C writer emits — a `goto` keyword followed by a space —
not labels and not the word inside comments alone:

```bash
# After a successful decompile to OUT.c
grep -c 'goto ' OUT.c || true
# Optional: list sites
grep -n 'goto ' OUT.c
```

`grep 'goto '` matches `goto lab_…;` from `CHLLWriter`. It does **not** count
leftover labels such as `lab_0x11c1:`. Do not count `goto` inside identifiers
or the diagnostic comment (that comment has no `goto ` token).

Suggested one-binary command (30s cap; do not wait for a rebuild):

```bash
DEC=build/linux/src/retdec-decompiler/retdec-decompiler
# If that path is a 0-byte ninja stub, use the install ELF instead:
# DEC=build/linux/install/bin/retdec-decompiler
BIN=tests/algorithm_recovery/corpus/binary_search-gcc-O0
timeout 30 "$DEC" "$BIN" -o /tmp/goto_baseline.c -s --disable-static-code-detection
grep -c 'goto ' /tmp/goto_baseline.c || true
```

`tests/decompilebench/corpus/*-gcc-O0` entries are often WSL symlinks; on
Windows they may be unreadable (OS error 1920). The algorithm-recovery corpus
files are real ELFs.

Stock RetDec: same `grep 'goto '` on that decompiler’s `.c` for the same
binary. Do not invent a stock number.

## Stock vs fork table

Fill a cell only from a run. Leave TBD otherwise.

| Binary | Stock `goto ` count | Fork `goto ` count | Notes |
|--------|---------------------|--------------------|-------|
| binary_search-gcc-O0 | TBD | **0** | Fork: 2026-08-22, ~5s WSL, `build/linux/install/bin/retdec-decompiler` (732 MB ELF, 2026-08-08). Requested `build/linux/src/retdec-decompiler/retdec-decompiler` was **empty (0 bytes)** — not rebuilt. Output 185 lines; `function_1169` is the search loop (while/if, leftover `lab_0x11c1:` label, no `goto `). Stock not run. |
| bubblesort-gcc-O0 | TBD | TBD | |
| memcpy_loop-gcc-O0 | TBD | TBD | |
| mergesort-gcc-O0 | TBD | TBD | |
| hash_table-gcc-O0 | TBD | TBD | |
| ring_buffer-gcc-O0 | TBD | TBD | |

Re-measure after a successful link of `build/linux/src/retdec-decompiler/retdec-decompiler` if that binary differs from the install copy.
