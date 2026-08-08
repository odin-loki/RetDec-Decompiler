# rellic evaluation (step 28)

**Library:** [rellic](https://github.com/lifting-bits/rellic) (Apache-2.0, Trail of Bits)  
**Goal:** Evaluate LLVM IR → C as an alternative to `llvmir2hll`.

## Why

`src/llvmir2hll/` is the 2019 bottleneck. Stock RetDec output quality is
largely determined here. rellic is actively maintained and may offer a path to
modern IR lowering without rewriting 95k lines immediately.

## Evaluation checklist

1. Build rellic against the same LLVM fork RetDec uses (`avast/llvm` @ LLVM 8 era).
2. Export LLVM bitcode from a RetDec lift for 10 corpus binaries.
3. Run rellic-decompile; compare syntax validity and recompile rate vs llvmir2hll.
4. Measure struct recovery and naming on algorithm-recovery subset.
5. Document licence compatibility (Apache-2.0 — OK for commercial with NOTICE).

## Script

```bash
bash scripts/eval_rellic.sh --corpus tests/decompilebench/corpus --out results/rellic-eval.json
```

## Decision criteria

| Outcome | Action |
|---------|--------|
| rellic ≥ llvmir2hll on recompile + readability | Plan incremental backend swap behind `RETDEC_ENABLE_RELLIC` |
| rellic wins on IR but loses on RetDec-specific types | Hybrid: rellic for lowering, RetDec for types/semantic export |
| rellic blocked on LLVM 8 | Defer until LLVM migration (step 33) |

## Companions

- **anvill** — lifted-binary IR with type recovery
- **remill** — instruction semantics → LLVM IR
