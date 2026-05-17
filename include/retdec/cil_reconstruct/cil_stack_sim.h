/**
 * @file include/retdec/cil_reconstruct/cil_stack_sim.h
 * @brief CIL typed stack simulation — Phase 2 of the CIL reconstruction pipeline.
 *
 * ## Overview
 *
 * CIL is a stack-based virtual machine.  Every instruction consumes and
 * produces typed values on an "evaluation stack".  Before we can emit C#
 * source, we need to understand:
 *
 *   1. The *type* of each value on the stack at each program point.
 *   2. How stack slots correspond to local variables (after coalescing).
 *   3. How to reconstruct compound expressions from instruction sequences.
 *
 * ## Stack simulation algorithm
 *
 * We perform a forward data-flow analysis over the BcCFG.  The state at
 * each basic block boundary is the operand-stack type sequence.
 *
 * ### Entry state
 *   - Normal entry: empty stack.
 *   - Catch handler entry: stack = [CatchType] (the exception object).
 *   - Filter handler entry: stack = [System.Object].
 *
 * ### Transfer function (per instruction)
 *
 * Each BcOpcode has a pop/push rule.  We apply them in order, checking
 * type compatibility at each pop.  For variable-effect instructions
 * (call*, newobj, ret):
 *   - Pop the declared parameter count.
 *   - Push the declared return type (or nothing for void).
 *
 * ### Meet (join point)
 *
 * When two paths merge, the stack depths must be equal and corresponding
 * slot types are unified using the CLR typing rules:
 *   - If both are identical: keep.
 *   - If one is a subtype of the other: use the wider (supertype).
 *   - Otherwise: use System.Object (safe widening).
 *
 * ## Stack slot / expression tree
 *
 * Each stack slot carries a `CilExpr` — an expression node that records
 * how the value was produced.  This lets us re-construct compound
 * expressions (e.g., `a + b` instead of temp0 = a; temp1 = b; temp2 = temp0 + temp1).
 *
 * `CilExpr` forms a small expression tree:
 *
 *   CilExpr ::= Const(val, type)
 *              | Local(idx, name, type)
 *              | Arg(idx, name, type)
 *              | Field(obj_expr, field_name, type)
 *              | SField(class_name, field_name, type)
 *              | Call(method, args, type)
 *              | Callvirt(obj, method, args, type)
 *              | Newobj(ctor, args, type)
 *              | Newarr(elem_type, len_expr)
 *              | Ldelem(arr, idx, type)
 *              | BinOp(op, lhs, rhs, type)
 *              | UnOp(op, operand, type)
 *              | Cast(expr, target_type)
 *              | Isinst(expr, target_type)
 *              | LdNull
 *              | LdStr(value)
 *              | LdToken(token_str)
 *              | Ternary(cond, then, else, type)
 *              | Box(expr, type)
 *              | Unbox(expr, type)
 *              | SizeOf(type)
 *              | AddressOf(expr)       // ldelema / ldflda / ldsflda / ldloca
 *              | Deref(expr, type)     // ldind.*
 *              | TypedRef(expr)        // typedref
 *              | Default(type)         // default(T)
 *              | Dup(expr)             // dup instruction
 */

#ifndef RETDEC_CIL_RECONSTRUCT_CIL_STACK_SIM_H
#define RETDEC_CIL_RECONSTRUCT_CIL_STACK_SIM_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/bc_module/bc_type.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace retdec {
namespace cil_reconstruct {

using namespace bc_module;

// ─── CilExpr — expression tree ────────────────────────────────────────────────

struct CilExpr;
using CilExprPtr = std::shared_ptr<CilExpr>;

// Binary operator kinds
enum class BinOpKind {
    Add, Sub, Mul, Div, DivUn, Rem, RemUn,
    And, Or, Xor, Shl, Shr, ShrUn,
    AddOvf, AddOvfUn, MulOvf, MulOvfUn, SubOvf, SubOvfUn,
    Eq, Ne, Lt, LtUn, Le, LeUn, Gt, GtUn, Ge, GeUn,
    // Pointer arithmetic
    PtrAdd,
};

// Unary operator kinds
enum class UnOpKind {
    Neg, Not,
    ConvI1, ConvU1, ConvI2, ConvU2,
    ConvI4, ConvU4, ConvI8, ConvU8,
    ConvR4, ConvR8, ConvI, ConvU,
    ConvR_Un,
    ConvOvfI1, ConvOvfU1, ConvOvfI2, ConvOvfU2,
    ConvOvfI4, ConvOvfU4, ConvOvfI8, ConvOvfU8,
    ConvOvfI1Un, ConvOvfU1Un, ConvOvfI2Un, ConvOvfU2Un,
    ConvOvfI4Un, ConvOvfU4Un, ConvOvfI8Un, ConvOvfU8Un,
    ConvOvfI, ConvOvfU, ConvOvfIUn, ConvOvfUUn,
};

// Node variants
struct ExprConst      { int64_t intVal; double fltVal; std::string strVal; BcType type; bool isFloat = false; bool isString = false; };
struct ExprNull       {};
struct ExprLocal      { uint32_t idx; std::string name; BcType type; };
struct ExprArg        { uint32_t idx; std::string name; BcType type; };
struct ExprField      { CilExprPtr obj; std::string className; std::string fieldName; BcType type; };
struct ExprSField     { std::string className; std::string fieldName; BcType type; };
struct ExprCall       { std::string className; std::string methodName; std::vector<CilExprPtr> args; BcType retType; bool isVirtual = false; CilExprPtr obj; }; // obj for instance calls
struct ExprNewobj     { std::string className; std::string ctorSig; std::vector<CilExprPtr> args; BcType type; };
struct ExprNewarr     { BcType elemType; CilExprPtr length; };
struct ExprLdelem     { CilExprPtr arr; CilExprPtr idx; BcType elemType; };
struct ExprBinOp      { BinOpKind op; CilExprPtr lhs; CilExprPtr rhs; BcType type; };
struct ExprUnOp       { UnOpKind op; CilExprPtr operand; BcType type; };
struct ExprCast       { CilExprPtr expr; BcType targetType; bool isChecked = false; };
struct ExprIsinst     { CilExprPtr expr; BcType targetType; };
struct ExprBox        { CilExprPtr expr; BcType boxedType; };
struct ExprUnbox      { CilExprPtr expr; BcType targetType; };
struct ExprSizeof     { BcType ofType; };
struct ExprAddressOf  { CilExprPtr expr; };
struct ExprDeref      { CilExprPtr addr; BcType type; };
struct ExprDup        { CilExprPtr expr; };
struct ExprLdToken    { std::string tokenStr; };
struct ExprLdFtn      { std::string className; std::string methodName; bool isVirtual = false; };
struct ExprDefault    { BcType type; };
struct ExprTernary    { CilExprPtr cond; CilExprPtr then; CilExprPtr elseBr; BcType type; };
struct ExprArgList    {};  // __arglist
struct ExprTypedRef   { CilExprPtr expr; };
struct ExprRefAnyVal  { CilExprPtr expr; BcType type; };
struct ExprRefAnyType { CilExprPtr expr; };
struct ExprMkRefAny   { CilExprPtr expr; BcType type; };
struct ExprLocAlloc   { CilExprPtr size; BcType elemType; };  // stackalloc

using CilExprVariant = std::variant<
    ExprConst, ExprNull, ExprLocal, ExprArg,
    ExprField, ExprSField,
    ExprCall, ExprNewobj, ExprNewarr, ExprLdelem,
    ExprBinOp, ExprUnOp,
    ExprCast, ExprIsinst, ExprBox, ExprUnbox,
    ExprSizeof, ExprAddressOf, ExprDeref,
    ExprDup, ExprLdToken, ExprLdFtn, ExprDefault, ExprTernary,
    ExprArgList, ExprTypedRef, ExprRefAnyVal, ExprRefAnyType,
    ExprMkRefAny, ExprLocAlloc
>;

struct CilExpr {
    CilExprVariant data;
    BcType         type;   // cached result type

    template<typename T>
    explicit CilExpr(T d, BcType t = BcType{}) : data(std::move(d)), type(std::move(t)) {}

    // Type query
    bool isConst()  const { return std::holds_alternative<ExprConst>(data); }
    bool isLocal()  const { return std::holds_alternative<ExprLocal>(data); }
    bool isArg()    const { return std::holds_alternative<ExprArg>(data); }
    bool isNull()   const { return std::holds_alternative<ExprNull>(data); }
    bool isCall()   const { return std::holds_alternative<ExprCall>(data); }
    bool isField()  const { return std::holds_alternative<ExprField>(data); }
    bool isSField() const { return std::holds_alternative<ExprSField>(data); }
    bool isBinOp()  const { return std::holds_alternative<ExprBinOp>(data); }
    bool isNewobj() const { return std::holds_alternative<ExprNewobj>(data); }
    bool isNewarr() const { return std::holds_alternative<ExprNewarr>(data); }

    const ExprConst&  asConst()  const { return std::get<ExprConst>(data); }
    const ExprLocal&  asLocal()  const { return std::get<ExprLocal>(data); }
    const ExprArg&    asArg()    const { return std::get<ExprArg>(data); }
    const ExprCall&   asCall()   const { return std::get<ExprCall>(data); }
    const ExprField&  asField()  const { return std::get<ExprField>(data); }
    const ExprSField& asSField() const { return std::get<ExprSField>(data); }
    const ExprBinOp&  asBinOp()  const { return std::get<ExprBinOp>(data); }
    const ExprNewobj& asNewobj() const { return std::get<ExprNewobj>(data); }
};

// ─── Stack slot ────────────────────────────────────────────────────────────────

/**
 * @brief One slot on the CIL evaluation stack.
 *
 * Carries the value's type and an optional expression tree for inline
 * expression reconstruction.
 */
struct StackSlot {
    BcType      type;   ///< Type of this stack slot
    CilExprPtr  expr;   ///< Expression that produced this value (may be null)

    bool operator==(const StackSlot& o) const noexcept { return type == o.type; }
    bool operator!=(const StackSlot& o) const noexcept { return !(*this == o); }
};

using StackState = std::vector<StackSlot>;

// ─── Stack frame (per basic block) ────────────────────────────────────────────

/**
 * @brief Records the stack state at each instruction boundary within a block.
 *
 * Used by the var recovery phase to identify pushes/pops and build
 * the expression tree.
 */
struct BlockStackInfo {
    StackState  entryStack;  ///< Stack at block entry (from predecessor meet)
    StackState  exitStack;   ///< Stack at block exit (after last instruction)
    /// Per-instruction output stacks (indexed by instruction position in block).
    std::vector<StackState> instrStacks;
};

// ─── CilStackSimulator ────────────────────────────────────────────────────────

/**
 * @brief Performs typed stack simulation over a CIL BcCFG.
 *
 * Produces:
 *   - Per-block `BlockStackInfo` (entry/exit/per-instruction stacks)
 *   - Type information for each stack slot at each instruction
 *
 * This is the input to the variable recovery phase.
 */
class CilStackSimulator {
public:
    struct Options {
        bool buildExprTrees = true;  ///< Build CilExpr nodes for expression reconstruction
        int  maxIterations  = 32;    ///< Max work-list iterations before giving up
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CilStackSimulator(const Options& opts = defaultOptions());

    /**
     * @brief Run stack simulation over a method's BcCFG.
     *
     * @param cfg        The basic block graph from the CIL lifter.
     * @param method     The BcMethod (for parameter types, etc.).
     * @param ehHandlers List of exception handlers (from the CLI header).
     * @return true on success; false if stack underflow or type conflict detected.
     */
    bool simulate(const BcCFG& cfg,
                  const BcMethod& method);

    bool isValid() const { return valid_; }
    const std::string& error() const { return error_; }

    /// Per-block stack information (indexed by block id).
    const BlockStackInfo& blockInfo(uint32_t blockId) const;

    /// Stack state at entry to block `blockId`.
    const StackState& entryStack(uint32_t blockId) const;

    /// Stack state after instruction at position `instrIdx` in block `blockId`.
    const StackState& instrStack(uint32_t blockId, uint32_t instrIdx) const;

    /// Expression produced by instruction `instrIdx` in block `blockId`
    /// (the top-of-stack expression after that instruction).
    CilExprPtr exprAt(uint32_t blockId, uint32_t instrIdx) const;

private:
    Options opts_;
    bool    valid_ = false;
    std::string error_;

    std::vector<BlockStackInfo> blockInfos_;

    // Work-list fixpoint loop
    bool runFixpoint(const BcCFG& cfg, const BcMethod& method);

    // Transfer function for one instruction
    bool applyInstruction(const BcInstruction& insn,
                           StackState& stack,
                           const BcMethod& method,
                           CilExprPtr& outExpr) const;

    // Stack meet (join)
    static StackState meetStates(const StackState& a, const StackState& b);

    // Type unification for CLR types
    static BcType unifyTypes(const BcType& a, const BcType& b);

    // Pop helpers
    static bool pop(StackState& s, StackSlot& out);
    static bool popN(StackState& s, int n, std::vector<StackSlot>& out);

    // Push helpers
    static void push(StackState& s, BcType t, CilExprPtr expr = nullptr);

    // Expression builders
    static CilExprPtr makeConst(int64_t v, BcType t);
    static CilExprPtr makeConst(double v);
    static CilExprPtr makeConst(const std::string& s);
    static CilExprPtr makeNull();
    static CilExprPtr makeLocal(uint32_t idx, const std::string& name, BcType t);
    static CilExprPtr makeArg(uint32_t idx, const std::string& name, BcType t);
    static CilExprPtr makeBinOp(BinOpKind op, CilExprPtr l, CilExprPtr r, BcType t);
    static CilExprPtr makeUnOp(UnOpKind op, CilExprPtr e, BcType t);
    static CilExprPtr makeCall(const BcInstruction& insn, std::vector<StackSlot> args, bool isVirtual);
    static CilExprPtr makeCast(CilExprPtr e, BcType t);
    static CilExprPtr makeIsinst(CilExprPtr e, BcType t);

    // CIL opcode → stack effect
    struct StackEffect { int pop; int push; }; // -1 = variable
    static StackEffect getStaticEffect(BcOpcode op);

    // Return type of a method reference operand
    static BcType methodReturnType(const BcInstruction& insn);
    static BcType fieldType(const BcInstruction& insn);
};

// ─── Factory helpers ──────────────────────────────────────────────────────────

inline CilExprPtr makeExprConst(int64_t v, BcType t) {
    ExprConst c; c.intVal = v; c.type = t;
    return std::make_shared<CilExpr>(std::move(c), t);
}

inline CilExprPtr makeExprNull() {
    return std::make_shared<CilExpr>(ExprNull{}, types::Null());
}

inline CilExprPtr makeExprLocal(uint32_t idx, std::string name, BcType t) {
    return std::make_shared<CilExpr>(ExprLocal{idx, std::move(name), t}, t);
}

inline CilExprPtr makeExprArg(uint32_t idx, std::string name, BcType t) {
    return std::make_shared<CilExpr>(ExprArg{idx, std::move(name), t}, t);
}

} // namespace cil_reconstruct
} // namespace retdec

#endif // RETDEC_CIL_RECONSTRUCT_CIL_STACK_SIM_H
