/**
 * @file src/idiom_reconstruct/abs_recovery.cpp
 * @brief Branchless absolute-value idiom recovery.
 *
 * ## Compiler-generated abs() sequences
 *
 * For signed integer abs(), compilers use one of three branchless forms
 * depending on target ISA and optimisation level:
 *
 * ### Form A — x86 CDQ form (GCC/Clang x86-32, MSVC /O1):
 *   CDQ              ; EDX:EAX = sign_extend(EAX)  → EDX = 0 or -1
 *   XOR EAX, EDX    ; EAX = EAX ^ (0 or -1)
 *   SUB EAX, EDX    ; EAX = EAX - (0 or -1) = |EAX|
 *
 *   In IdiomInstr terms:
 *   [0]  CDQ   dst=edx, src0=eax          ; sign_extend
 *   [1]  XOR   dst=eax, src0=eax, src1=edx
 *   [2]  SUB   dst=eax, src0=eax, src1=edx
 *
 * ### Form B — shift-xor-sub (GCC/Clang, most ISAs):
 *   t = x >> (N-1)           ; SAR eax, 31 → t = 0 or -1
 *   r = (x ^ t) - t          ; XOR + SUB in one expression
 *
 *   In IdiomInstr terms:
 *   [0]  SAR   t, x, N-1
 *   [1]  XOR   t2, x, t
 *   [2]  SUB   r,  t2, t
 *
 * ### Form C — AArch64 / ARM32 form (LLVM AArch64 backend):
 *   NEG   t, x              ; t = -x
 *   CSEL  r, x, t, ge       ; select x if x >= 0, else t
 *   (or equivalent CSNEG)
 *   Mapped to our IdiomOp as:
 *   [0]  Neg   t, x
 *   [1]  Mov   r, csel(x, t, cond_ge)   — too ISA-specific, skip for now
 *
 * ### Form D — Clang alternative for 64-bit (uses MOVSX/SAR/XOR/SUB in sequence):
 *   MOVSX  t64, x32         ; zero/sign-extend
 *   SAR    t2, t64, 63
 *   XOR    t3, x, t2
 *   SUB    r,  t3, t2
 *   Functionally identical to Form B with N=64.
 *
 * ### Form E — MSVC ARM (uses ASR + EOR + RSB):
 *   ASR    t, x, 31
 *   EOR    t2, x, t
 *   RSB    r,  t, t2        ; r = t2 - t = |x|
 *   Semantically same as Form B, SUB operands reversed.
 *
 * We match all of the above.
 */

#include <memory>
#include "retdec/idiom_reconstruct/idiom_reconstruct.h"
#include <cstdint>
#include <optional>

namespace retdec {
namespace idiom_reconstruct {

namespace {

class AbsMatcher : public IIdiomMatcher {
public:
    const char* name() const noexcept override { return "BranchlessAbs"; }
    std::size_t minWindowSize() const noexcept override { return 3; }

    std::optional<ReplacementNode> match(const InstrWindow& W,
                                          std::size_t        off,
                                          CompilerProfile    /*prof*/) const override
    {
        const std::size_t n = W.size();

        // ── Form A: CDQ; XOR dst, dst, edx; SUB dst, dst, edx ────────────────
        if (off+2 < n && W[off].op==IdiomOp::Cdq) {
            uint32_t signReg  = W[off].dst.reg;   // EDX (the sign extension)
            uint32_t xReg     = W[off].src0.reg;  // EAX (the input)
            // XOR xReg, xReg, signReg
            if (W[off+1].op==IdiomOp::Xor &&
                W[off+1].dst.reg==xReg &&
                ((W[off+1].src0.reg==xReg && W[off+1].src1.reg==signReg) ||
                 (W[off+1].src0.reg==signReg && W[off+1].src1.reg==xReg)))
            {
                uint32_t xorDst = W[off+1].dst.reg;
                // SUB xorDst, xorDst, signReg
                if (W[off+2].op==IdiomOp::Sub &&
                    W[off+2].src0.reg==xorDst &&
                    W[off+2].src1.reg==signReg)
                {
                    ReplacementNode r;
                    r.kind        = ReplacementKind::AbsValue;
                    r.inputReg    = xReg;
                    r.outputReg   = W[off+2].dst.reg;
                    r.operandWidth= W[off].src0.width;
                    r.firstVma    = W[off].vma;
                    r.lastVma     = W[off+2].vma;
                    r.instrCount  = 3;
                    return r;
                }
            }
        }

        // ── Form B: SAR t, x, N-1; XOR t2, x, t; SUB r, t2, t ───────────────
        if (off+2 < n && W[off].op==IdiomOp::Sar && W[off].isImm(1)) {
            int N = (int)W[off].src0.width;
            if (W[off].getImm(1) == N-1) {
                uint32_t xReg = W[off].src0.reg;
                uint32_t tReg = W[off].dst.reg;   // sign mask (0 or -1)
                // XOR t2, x, t  (order may vary)
                if (W[off+1].op==IdiomOp::Xor &&
                    ((W[off+1].src0.reg==xReg && W[off+1].src1.reg==tReg) ||
                     (W[off+1].src0.reg==tReg && W[off+1].src1.reg==xReg)))
                {
                    uint32_t t2 = W[off+1].dst.reg;
                    // SUB r, t2, t
                    if (W[off+2].op==IdiomOp::Sub &&
                        W[off+2].src0.reg==t2 &&
                        W[off+2].src1.reg==tReg)
                    {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::AbsValue;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+2].dst.reg;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+2].vma;
                        r.instrCount  = 3;
                        return r;
                    }
                    // Form E (ARM RSB): SUB r, t, t2  (reversed operands)
                    if (W[off+2].op==IdiomOp::Sub &&
                        W[off+2].src0.reg==tReg &&
                        W[off+2].src1.reg==t2)
                    {
                        // RSB r, t, t2 = t2 - t (semantically same)
                        ReplacementNode r;
                        r.kind        = ReplacementKind::AbsValue;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+2].dst.reg;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+2].vma;
                        r.instrCount  = 3;
                        return r;
                    }
                }
                // Also: ADD variant: SAR t, x, N-1; ADD t2, x, t; XOR r, t2, t
                // (used by some ARM/RISC-V toolchains)
                if (off+2 < n &&
                    W[off+1].op==IdiomOp::Add &&
                    ((W[off+1].src0.reg==xReg && W[off+1].src1.reg==tReg) ||
                     (W[off+1].src0.reg==tReg && W[off+1].src1.reg==xReg)))
                {
                    uint32_t t2 = W[off+1].dst.reg;
                    if (W[off+2].op==IdiomOp::Xor &&
                        ((W[off+2].src0.reg==t2 && W[off+2].src1.reg==tReg) ||
                         (W[off+2].src0.reg==tReg && W[off+2].src1.reg==t2)))
                    {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::AbsValue;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+2].dst.reg;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+2].vma;
                        r.instrCount  = 3;
                        return r;
                    }
                }
            }
        }

        // ── Form D: MOVSX t64, x32; SAR t2, t64, 63; XOR t3, x, t2; SUB r, t3, t2 ──
        if (off+3 < n &&
            (W[off].op==IdiomOp::Movsx || W[off].op==IdiomOp::Movzx))
        {
            uint32_t extDst = W[off].dst.reg;
            uint32_t xReg   = W[off].src0.reg;
            int N = 64; // extension produces 64-bit
            if (W[off+1].op==IdiomOp::Sar && W[off+1].isImm(1) &&
                W[off+1].getImm(1)==N-1 &&
                W[off+1].src0.reg==extDst)
            {
                uint32_t tReg = W[off+1].dst.reg;
                if (W[off+2].op==IdiomOp::Xor &&
                    ((W[off+2].src0.reg==xReg && W[off+2].src1.reg==tReg) ||
                     (W[off+2].src0.reg==tReg && W[off+2].src1.reg==xReg)))
                {
                    uint32_t t3 = W[off+2].dst.reg;
                    if (W[off+3].op==IdiomOp::Sub &&
                        W[off+3].src0.reg==t3 &&
                        W[off+3].src1.reg==tReg)
                    {
                        ReplacementNode r;
                        r.kind        = ReplacementKind::AbsValue;
                        r.inputReg    = xReg;
                        r.outputReg   = W[off+3].dst.reg;
                        r.operandWidth= (uint32_t)N;
                        r.firstVma    = W[off].vma;
                        r.lastVma     = W[off+3].vma;
                        r.instrCount  = 4;
                        return r;
                    }
                }
            }
        }

        return std::nullopt;
    }
};

} // anon namespace

std::unique_ptr<IIdiomMatcher> makeAbsMatcher() {
    return std::make_unique<AbsMatcher>();
}

} // namespace idiom_reconstruct
} // namespace retdec
