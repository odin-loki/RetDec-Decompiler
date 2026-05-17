/**
 * @file include/retdec/csharp_emitter/cs_expr_emitter.h
 * @brief CilExpr → C# expression text.
 *
 * Converts every `CilExpr` variant to syntactically correct C# source.
 * Handles operator precedence and parenthesisation automatically.
 *
 * ## Precedence levels (matching C# spec §7.3.1)
 *
 *   14  Primary:          x.y  f(x)  a[x]  x++  x--  new  typeof  sizeof  checked  unchecked
 *   13  Unary:            +x  -x  !x  ~x  ++x  --x  (T)x  await
 *   12  Multiplicative:   *  /  %
 *   11  Additive:         +  -
 *   10  Shift:            <<  >>  >>>
 *    9  Relational/type:  <  >  <=  >=  is  as
 *    8  Equality:         ==  !=
 *    7  Bitwise AND:      &
 *    6  Bitwise XOR:      ^
 *    5  Bitwise OR:       |
 *    4  Logical AND:      &&
 *    3  Logical OR:       ||
 *    2  Null-coalescing:  ??
 *    1  Conditional:      ?:
 *    0  Assignment:       =  +=  -=  …
 */

#ifndef RETDEC_CSHARP_EMITTER_CS_EXPR_EMITTER_H
#define RETDEC_CSHARP_EMITTER_CS_EXPR_EMITTER_H

#include "retdec/cil_reconstruct/cil_stack_sim.h"
#include "retdec/csharp_emitter/cs_writer.h"

#include <string>

namespace retdec {
namespace csharp_emitter {

using namespace cil_reconstruct;

// ─── CsExprEmitter ───────────────────────────────────────────────────────────

/**
 * @brief Renders a CilExpr tree as a C# expression string.
 *
 * The entry point is `emit(expr)`.  All helper methods are private.
 * A single `CsExprEmitter` instance is stateless between calls and can
 * be shared by the stmt and type emitters.
 */
class CsExprEmitter {
public:
    struct Options {
        bool useVarKeyword        = true;   ///< Use `var` where type is obvious
        bool preferStringInterp   = true;   ///< Use $"" for simple Format() calls
        bool emitExplicitCasts    = true;   ///< Emit (T)x even for upcast
        bool nullableAnnotations  = true;   ///< Emit ? for nullable reference types
        int  csharpVersion        = 12;
    };
    static Options defaultOptions() noexcept { return {}; }

    explicit CsExprEmitter(CsWriter& writer, Options opts = defaultOptions());

    /**
     * @brief Emit `expr` as a C# expression string and return it.
     *
     * Does NOT write to the CsWriter directly — callers decide where the
     * expression appears.
     *
     * @param parentPrec  Operator precedence of the parent context (0 = top).
     *                    If the expression's own precedence is lower, it will
     *                    be wrapped in parentheses.
     */
    std::string emit(const CilExprPtr& expr, int parentPrec = 0) const;

    /// Emit a BcType as a C# type name.
    std::string emitType(const BcType& type) const;

    /// Emit a list of args separated by ", "
    std::string emitArgList(const std::vector<CilExprPtr>& args) const;

private:
    CsWriter& writer_;
    Options   opts_;

    // ── Dispatch ─────────────────────────────────────────────────────────────

    std::string emitConst(const ExprConst& e) const;
    std::string emitNull()                    const;
    std::string emitLocal(const ExprLocal& e) const;
    std::string emitArg(const ExprArg& e)     const;
    std::string emitField(const ExprField& e, int prec) const;
    std::string emitSField(const ExprSField& e)         const;
    std::string emitCall(const ExprCall& e, int prec)   const;
    std::string emitNewobj(const ExprNewobj& e)         const;
    std::string emitNewarr(const ExprNewarr& e)         const;
    std::string emitLdelem(const ExprLdelem& e, int prec) const;
    std::string emitBinOp(const ExprBinOp& e, int prec)  const;
    std::string emitUnOp(const ExprUnOp& e, int prec)    const;
    std::string emitCast(const ExprCast& e, int prec)    const;
    std::string emitIsinst(const ExprIsinst& e, int prec) const;
    std::string emitBox(const ExprBox& e, int prec)      const;
    std::string emitUnbox(const ExprUnbox& e, int prec)  const;
    std::string emitSizeof(const ExprSizeof& e)          const;
    std::string emitAddressOf(const ExprAddressOf& e, int prec) const;
    std::string emitDeref(const ExprDeref& e, int prec)  const;
    std::string emitDup(const ExprDup& e, int prec)      const;
    std::string emitLdToken(const ExprLdToken& e)        const;
    std::string emitLdFtn(const ExprLdFtn& e)            const;
    std::string emitDefault(const ExprDefault& e)        const;
    std::string emitTernary(const ExprTernary& e, int prec) const;
    std::string emitLocAlloc(const ExprLocAlloc& e)      const;
    std::string emitMkRefAny(const ExprMkRefAny& e, int prec) const;

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// Map BinOpKind → C# operator token and its precedence level.
    static std::pair<std::string, int> binOpInfo(BinOpKind op);

    /// Map UnOpKind → C# conversion/operator expression.
    static std::string unOpText(UnOpKind op, const std::string& operand);

    /// Wrap `text` in parentheses if `exprPrec < parentPrec`.
    static std::string parenIf(const std::string& text, int exprPrec, int parentPrec);

    /// Shorten a fully-qualified type name using C# aliases and the current namespace.
    std::string shortenType(const std::string& fqn) const;

    /// True if the type name is a nullable value type (ends in "?").
    static bool isNullableValueType(const std::string& typeName);
};

} // namespace csharp_emitter
} // namespace retdec

#endif // RETDEC_CSHARP_EMITTER_CS_EXPR_EMITTER_H
