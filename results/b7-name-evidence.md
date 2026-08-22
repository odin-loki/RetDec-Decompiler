# B7 — name evidence vs headline F1

Audit B7: a detector may consult a symbol name only with an explicit
`evidence:symbol_name` tag that reaches config JSON and is excluded from
headline algorithm-recovery extract.

This is **not** a product F1 change by itself.

## Shipped

- Concurrency export (`thread` / `mutex` / `atomic` / `condition_variable` /
  `spinlock`) writes `evidence:symbol_name` in `detail`. Those hits are
  import-table / callee-name matches (`concurrency_detect`).
- `scripts/extract_decompiler_predictions.py` skips any detection whose
  `detail` contains `evidence:symbol_name`.
- Test: `test_symbol_name_evidence_excluded_from_headline`.
- ci-core remasure after concurrency tag: mean F1 **0.237** (was 0.332).
  `generated_pthread_mutex-gcc-O0` is 0.000.
- Open-addressing export also tags `evidence:symbol_name` (`hasKeyCall`).
  Remasure: full 216 mean F1 **0.056** (was 0.107); ci-core **0.126**.
  `hash_table-gcc-O0` is 0.000.

A8 (lock-prefix / ldxr) is still blocked: `IrInstr::Op` has no lock/atomic.

## Not tagged yet (leftover)

These still read `calleeName` or a symbol table and mix that into the same
confidence as structural evidence:

- `serial_detect` (`symContains` on SerializeToString, FlatBuffer, …)
- `unordered_detect` (hash callee *or* xor+mul; not tagged because the
  xor+mul path is structural)
- `sort_detect` (self-name recursion, `_introsort`)
- `algo_recover` partition `swap` callee; idiom self-recursion
- `crypto_detect` / `pattern_detect` callee-name tables

Input-path reads in `retdec.cpp` `tryEmulationUnpacking` are diagnostic
logs only (`RETDEC_EMULATION_UNPACK_DIAG`), not detector scores.
