# Retypd, SAILR, LLVM migration (steps 30–33)

Human-led roadmap items. Do not hand to Composer without tight specs.

## Step 30 — Retypd (3–6 months)

**Paper:** SURE'25 expectations apply — benchmark before committing.  
**Goal:** Replace or augment RetDec type inference with Retypd's constraint-based recovery.

### Entry criteria

- Algorithm-recovery corpus wired (step 10)
- Baseline struct-recovery metrics from DecompileBench

### Milestones

1. Spike: Retypd on 10 LLVM modules lifted by RetDec
2. Compare pointer/struct recovery vs current `type_inference` pass
3. Go/no-go at month 3

## Step 31 — SAILR (3–6 months)

**Goal:** Structure recovery via SAILR-style lifting.  
**Blocked on:** LLVM version alignment (step 33) or isolated IR export path.

## Step 32 — Neural tiers 4–5

Tiers 4 (idiom recovery) and 5 (full rewrite) are enabled via `RETDEC_NEURAL_TIER_MAX=5`.
Requires frontier-model review of gate architecture (Part 14.7).

## Step 33 — LLVM migration (6+ months)

**Current:** `avast/llvm` @ LLVM 8 era — 314 files reference `llvm::`.  
**Target:** Modern LLVM LTS (22.x) or fork aligned with rellic/anvill stack.

### Preconditions

- Docker baseline green on new toolchain
- Retypd evaluation complete or explicitly deferred
- Full ctest green on LLVM 8 baseline tagged

### Approach

1. Inventory breaking API uses (`getPointerElementType`, implicit `CreateLoad`)
2. One pass at a time behind `RETDEC_LLVM_NEXT` flag
3. Never bump `deps/llvm` in autonomous Composer runs (Part 14)
