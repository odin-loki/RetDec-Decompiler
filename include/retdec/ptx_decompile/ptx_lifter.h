/**
 * @file include/retdec/ptx_decompile/ptx_lifter.h
 * @brief PTX-to-CUDA-C Lifter — Stage 43.
 *
 * Parses NVIDIA PTX assembly (text format as emitted by `cuobjdump --dump-ptx`)
 * and lifts it to CUDA C source code.
 *
 * ## PTX coverage
 *
 *   ### Types
 *     .b8/.b16/.b32/.b64, .s8/.s16/.s32/.s64, .u8/.u16/.u32/.u64,
 *     .f16/.f32/.f64, .pred (predicate register).
 *
 *   ### Memory spaces
 *     .global  → global memory         → regular pointer / __device__
 *     .shared  → shared memory         → __shared__
 *     .local   → thread-local memory   → stack variable
 *     .const   → constant memory       → __constant__
 *     .param   → kernel parameter      → function argument
 *     .reg     → virtual register
 *
 *   ### Thread indices recovered
 *     %tid.x/y/z    → threadIdx.x/y/z
 *     %ntid.x/y/z   → blockDim.x/y/z
 *     %ctaid.x/y/z  → blockIdx.x/y/z
 *     %nctaid.x/y/z → gridDim.x/y/z
 *     %laneid       → (threadIdx.x & 31)
 *     %warpid       → (threadIdx.x >> 5)
 *     %clock        → clock()
 *     %clock64      → clock64()
 *
 *   ### Instructions mapped
 *     mov, add, sub, mul, mad, fma, div, rem, neg, abs, min, max
 *     shl, shr, and, or, xor, not
 *     setp (→ CUDA comparison in if condition)
 *     selp (→ ternary `? :`)
 *     ld, st (→ dereference / assignment through pointer)
 *     cvt (→ C cast)
 *     bar.sync (→ __syncthreads())
 *     membar.gl (→ __threadfence())
 *     membar.cta (→ __threadfence_block())
 *     atom (→ atomicAdd / atomicCAS etc.)
 *     vote.all/any/ballot (→ __all_sync / __any_sync / __ballot_sync)
 *     shfl (→ __shfl_sync / __shfl_up_sync / __shfl_down_sync / __shfl_xor_sync)
 *     bfe, bfi (→ bit-field extract/insert helpers)
 *     sqrt.approx (→ __fsqrt_rn / sqrtf)
 *     rsqrt.approx (→ __frsqrt_rn / rsqrtf)
 *     sin.approx (→ __sinf), cos.approx (→ __cosf)
 *     ret → return
 *     bra → goto / structured if/else (after control-flow structuring)
 *     call → function call
 *
 * ## Output
 *
 *   PtxKernel — parsed representation of one PTX kernel
 *   PtxLifter — lifts PtxKernel to CUDA C text
 *
 * ## Example
 *
 *   PTX:
 *     .visible .entry vectorAdd(.param .u64 a, .param .u64 b, .param .u64 c, .param .u32 n)
 *     {
 *       .reg .u32  %r<4>;
 *       .reg .u64  %rd<4>;
 *       .reg .f32  %f<3>;
 *       ld.param.u64 %rd0, [a];
 *       ...
 *       bar.sync 0;
 *       ret;
 *     }
 *
 *   CUDA C output:
 *     __global__ void vectorAdd(unsigned long long* a, unsigned long long* b,
 *                               unsigned long long* c, unsigned int n) {
 *       const int tid = blockIdx.x * blockDim.x + threadIdx.x;
 *       unsigned int r0, r1, r2, r3;
 *       unsigned long long rd0, rd1, rd2, rd3;
 *       float f0, f1, f2;
 *       rd0 = (unsigned long long)a;
 *       ...
 *       __syncthreads();
 *     }
 */

#ifndef RETDEC_PTX_DECOMPILE_PTX_LIFTER_H
#define RETDEC_PTX_DECOMPILE_PTX_LIFTER_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace retdec::ptx_decompile {

// ─── PTX types ────────────────────────────────────────────────────────────────

enum class PtxType {
    Unknown,
    Pred,           ///< .pred — predicate
    B8, B16, B32, B64,
    S8, S16, S32, S64,
    U8, U16, U32, U64,
    F16, F32, F64,
};

enum class PtxSpace {
    Reg,    ///< .reg
    Global, ///< .global
    Shared, ///< .shared
    Local,  ///< .local
    Const,  ///< .const
    Param,  ///< .param
    Unknown,
};

// ─── PTX operand ──────────────────────────────────────────────────────────────

enum class PtxOperandKind {
    Register,    ///< %r3, %rd2, %f0, %p0
    Immediate,   ///< 42, 0x1F, 3.14
    Address,     ///< [%rd0+8] or [symbol]
    SpecialReg,  ///< %tid.x, %ctaid.y, etc.
    Label,       ///< for branch targets
};

struct PtxOperand {
    PtxOperandKind kind    = PtxOperandKind::Immediate;
    std::string    name;       ///< register/label name
    int64_t        immVal  = 0;
    double         fltVal  = 0.0;
    int64_t        offset  = 0;     ///< for address operands
    bool           isFloat = false;
};

// ─── PTX instruction ──────────────────────────────────────────────────────────

struct PtxInstr {
    std::string              mnemonic;     ///< e.g. "ld", "add", "bar"
    std::string              modifier;     ///< e.g. ".global.u32", ".sync"
    std::string              predReg;      ///< @%p0 guard predicate
    bool                     negPred = false;
    PtxType                  type    = PtxType::Unknown;
    std::vector<PtxOperand>  operands;
    std::string              label;        ///< label defined at this instr
    uint64_t                 offset  = 0;  ///< byte offset in PTX listing
};

// ─── PTX variable declaration ─────────────────────────────────────────────────

struct PtxVarDecl {
    PtxSpace    space = PtxSpace::Reg;
    PtxType     type  = PtxType::Unknown;
    std::string name;
    int         count = 1;   ///< for vectorised regs (%r<4> → r0..r3)
    int         align = 0;
    size_t      bytes = 0;   ///< for .shared/.local arrays
};

// ─── PTX parameter ────────────────────────────────────────────────────────────

struct PtxParam {
    PtxType     type = PtxType::Unknown;
    PtxSpace    space = PtxSpace::Param;
    std::string name;
    bool        isPointer = false;
};

// ─── PTX kernel ───────────────────────────────────────────────────────────────

enum class PtxKernelKind {
    Entry,   ///< .visible .entry — __global__
    Func,    ///< .visible .func  — __device__
};

struct PtxKernel {
    PtxKernelKind          kind   = PtxKernelKind::Entry;
    std::string            name;
    std::vector<PtxParam>  params;
    std::vector<PtxVarDecl>decls;
    std::vector<PtxInstr>  instrs;
};

// ─── PTX module ───────────────────────────────────────────────────────────────

struct PtxModule {
    int                       smVersion = 0;  ///< .target sm_XX
    std::vector<PtxKernel>    kernels;
    std::vector<PtxVarDecl>   globals;   ///< .global declarations
    std::vector<std::string>  constVars; ///< .const declarations
};

// ─── PTX parser ───────────────────────────────────────────────────────────────

/**
 * @brief Parses PTX text into a PtxModule.
 *
 * Handles the subset of PTX generated by nvcc for typical CUDA kernels.
 * Unknown directives and instructions are skipped with a warning.
 */
class PtxParser {
public:
    /**
     * @brief Parse PTX source text.
     * @return Parsed module. On error, module.kernels is empty and
     *         lastError() is set.
     */
    PtxModule parse(const std::string& ptxSource);

    const std::string& lastError() const { return lastError_; }

private:
    std::string lastError_;

    PtxType     parseType     (const std::string& tok) const;
    PtxSpace    parseSpace    (const std::string& tok) const;
    PtxOperand  parseOperand  (const std::string& tok) const;
    PtxInstr    parseInstr    (const std::vector<std::string>& tokens) const;
    PtxVarDecl  parseVarDecl  (const std::vector<std::string>& tokens) const;
    PtxParam    parseParam    (const std::vector<std::string>& tokens) const;

    static std::vector<std::string> tokenise(const std::string& line);
};

// ─── Thread index recovery ────────────────────────────────────────────────────

/**
 * @brief Replaces PTX special-register accesses with CUDA built-in names.
 *
 *   %tid.x          → threadIdx.x
 *   %tid.y          → threadIdx.y
 *   %tid.z          → threadIdx.z
 *   %ntid.x         → blockDim.x
 *   %ctaid.x        → blockIdx.x
 *   %nctaid.x       → gridDim.x
 *   %laneid         → (threadIdx.x & 31)
 *   %warpid         → (threadIdx.x >> 5)
 *   %lanemask_lt    → __lanemask_lt()
 *   %lanemask_gt    → __lanemask_gt()
 *   %smid           → 0   (SM id — not available in CUDA C)
 */
class ThreadIndexRecovery {
public:
    /**
     * @brief Returns the CUDA C expression for a PTX special register name.
     *        Returns empty string if not a special register.
     */
    static std::string resolve(const std::string& ptxRegName);
    static bool        isSpecial(const std::string& ptxRegName);
};

// ─── Shared memory declaration ────────────────────────────────────────────────

/**
 * @brief Generates __shared__ declarations from .shared variable declarations.
 */
class SharedMemDeclaration {
public:
    /**
     * @brief Emit CUDA C __shared__ declaration for a .shared variable.
     *        E.g.: __shared__ float smem[256];
     */
    static std::string emit(const PtxVarDecl& decl);
};

// ─── Instruction lifter ───────────────────────────────────────────────────────

/**
 * @brief Lifts individual PTX instructions to CUDA C expressions.
 */
class InstrLifter {
public:
    /**
     * @brief Lift one PTX instruction.
     * @param instr  The PTX instruction.
     * @param indent Indentation string.
     * @return One or more CUDA C statement strings.
     */
    std::string lift(const PtxInstr& instr, const std::string& indent = "    ") const;

private:
    std::string liftLoad        (const PtxInstr& i, const std::string& ind) const;
    std::string liftStore       (const PtxInstr& i, const std::string& ind) const;
    std::string liftArith       (const PtxInstr& i, const std::string& ind) const;
    std::string liftSetp        (const PtxInstr& i, const std::string& ind) const;
    std::string liftSelp        (const PtxInstr& i, const std::string& ind) const;
    std::string liftCvt         (const PtxInstr& i, const std::string& ind) const;
    std::string liftBar         (const PtxInstr& i, const std::string& ind) const;
    std::string liftMembar      (const PtxInstr& i, const std::string& ind) const;
    std::string liftAtom        (const PtxInstr& i, const std::string& ind) const;
    std::string liftVote        (const PtxInstr& i, const std::string& ind) const;
    std::string liftShfl        (const PtxInstr& i, const std::string& ind) const;
    std::string liftMath        (const PtxInstr& i, const std::string& ind) const;
    std::string liftBranch      (const PtxInstr& i, const std::string& ind) const;
    std::string liftCall        (const PtxInstr& i, const std::string& ind) const;
    std::string liftRet         (const PtxInstr& i, const std::string& ind) const;
    std::string liftMov         (const PtxInstr& i, const std::string& ind) const;

    static std::string operandStr(const PtxOperand& op, PtxType t);
    static std::string typeStr   (PtxType t);
    static std::string cTypeStr  (PtxType t);
    static std::string sepCmp    (const std::string& cmpMod);
};

// ─── PtxLifter ────────────────────────────────────────────────────────────────

/**
 * @brief Lifts a complete PtxKernel to CUDA C source code.
 */
class PtxLifter {
public:
    /**
     * @brief Lift one kernel to CUDA C.
     */
    std::string liftKernel(const PtxKernel& kernel) const;

    /**
     * @brief Lift an entire PTX module (all kernels) to CUDA C.
     */
    std::string liftModule(const PtxModule& mod) const;

private:
    std::string emitSignature(const PtxKernel& kernel) const;
    std::string emitDecls    (const PtxKernel& kernel) const;
    std::string emitBody     (const PtxKernel& kernel) const;

    static std::string paramTypeStr(const PtxParam& p);
};

} // namespace retdec::ptx_decompile

#endif // RETDEC_PTX_DECOMPILE_PTX_LIFTER_H
