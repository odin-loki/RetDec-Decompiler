/**
 * @file include/retdec/java_emitter/java_expr_emitter.h
 * @brief Java expression emitter.
 *
 * Converts expression-level BcInstructions (plus the PatternLiftResult
 * annotations from the reconstruction pipeline) into syntactically valid
 * Java expression strings.
 *
 * ## Expression forms supported
 *
 *   Literals:   42, 42L, 3.14f, 3.14, true, false, null, "string", 'c'
 *   Variables:  local names from BcLocalVar table
 *   Arithmetic: +, -, *, /, %, << (signed), >> (signed), >>> (unsigned)
 *   Bitwise:    &, |, ^, ~
 *   Boolean:    &&, ||, !
 *   Comparison: ==, !=, <, <=, >, >=
 *   Cast:       (Type) expr
 *   instanceof: expr instanceof Type
 *               expr instanceof Type varName   (Java 16+ pattern variable)
 *   new:        new Foo(args)
 *               new int[n]
 *               new String[]{"a","b"}
 *   Field:      obj.field, Cls.staticField
 *   Method:     obj.method(args), Cls.staticMethod(args)
 *   Array:      arr[i]
 *   Ternary:    cond ? a : b
 *   Assignment: x = expr, x += expr, etc.
 *   Lambda:     (T x) -> body, x -> expr
 *   MethodRef:  Cls::method, obj::method, Cls::new
 *   String+:    a + b  (strings)
 *   Increment:  ++x, x++, --x, x--
 */

#ifndef RETDEC_JAVA_EMITTER_JAVA_EXPR_EMITTER_H
#define RETDEC_JAVA_EMITTER_JAVA_EXPR_EMITTER_H

#include "retdec/bc_module/bc_cfg.h"
#include "retdec/bc_module/bc_instr.h"
#include "retdec/bc_module/bc_module.h"
#include "retdec/java_emitter/java_type_printer.h"
#include "retdec/jvm_reconstruct/jvm_reconstruct.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace java_emitter {

using namespace bc_module;
using namespace jvm_reconstruct;

// ─── Emitter context ─────────────────────────────────────────────────────────

/**
 * @brief Shared state passed down through the expression emitter.
 */
struct ExprContext {
    const BcMethod&          method;
    const ReconstructResult& recon;
    const JavaTypePrinter&   tyPrinter;

    /// Map from local variable index → name (from BcMethod::locals).
    std::unordered_map<uint32_t, std::string> localNames;
    std::unordered_map<uint32_t, BcType>      localTypes;

    ExprContext(const BcMethod& m,
                const ReconstructResult& r,
                const JavaTypePrinter& tp);
};

// ─── Expression node (tree form) ─────────────────────────────────────────────

/**
 * @brief An expression tree node produced by the expression emitter.
 *
 * We build a lightweight AST so that:
 *  - Parenthesisation can be done correctly via operator precedence.
 *  - Lambda bodies can be checked for single-expression form.
 */
enum class ExprKind {
    Literal,        ///< integer/float/bool/null/string literal
    LocalVar,       ///< named local variable reference
    FieldAccess,    ///< obj.field or Cls.field
    ArrayAccess,    ///< arr[idx]
    MethodCall,     ///< obj.method(args) or Cls.method(args)
    NewObject,      ///< new Cls(args)
    NewArray,       ///< new T[n]  or  new T[]{...}
    Cast,           ///< (T) expr
    Instanceof,     ///< expr instanceof T [varName]
    Unary,          ///< -x, !x, ~x, ++x, x++, etc.
    Binary,         ///< x + y, x == y, etc.
    Ternary,        ///< cond ? a : b
    Assign,         ///< x = expr  or  x += expr
    Lambda,         ///< (params) -> body
    MethodRef,      ///< Cls::method
    StringConcat,   ///< folded string + chain
};

struct ExprNode {
    ExprKind    kind;
    std::string text;           ///< Fully emitted text (leaf or composed)
    int         precedence = 0; ///< Operator precedence (higher = tighter binding)
    bool        sideEffects = false;
};

// ─── Java expression emitter ─────────────────────────────────────────────────

class JavaExprEmitter {
public:
    explicit JavaExprEmitter(ExprContext& ctx);

    /**
     * @brief Emit an expression for a single BcInstruction.
     *
     * For stack-based instructions (after reconstruction), the operand
     * expressions are pulled from the `exprStack`. For variable-based
     * instructions (LoadLocal/StoreLocal), operands are looked up directly.
     *
     * @param insn   The instruction to emit.
     * @param stack  Mutable expression stack (pop inputs, push result).
     * @return The emitted expression string, or "" if the instruction
     *         produces a statement (StoreLocal, void invoke, etc.).
     */
    std::string emitInsn(const BcInstruction& insn,
                          std::vector<ExprNode>& stack);

    /**
     * @brief Emit a lambda expression from a LambdaPattern.
     */
    std::string emitLambda(const LambdaPattern& pat,
                            const BcClass& ownerClass);

    /**
     * @brief Emit a method reference from a LambdaPattern.
     */
    std::string emitMethodRef(const LambdaPattern& pat);

    /**
     * @brief Emit a string concatenation expression.
     */
    std::string emitStringConcat(const StringConcatPattern& pat,
                                  const std::vector<ExprNode>& parts);

private:
    ExprContext& ctx_;

    // Parenthesise `expr` if its precedence is lower than `required`.
    static std::string paren(const ExprNode& expr, int required);

    // Operator precedence table (higher = binds tighter).
    static int precedenceOf(BcOpcode op);

    // Map BcOpcode to Java operator symbol.
    static std::string opSymbol(BcOpcode op);

    // Emit literal from a PushInt / PushFloat / PushString instruction.
    static std::string emitLiteral(const BcInstruction& insn);

    // Emit a method call (virtual, static, interface, special).
    std::string emitMethodCall(const BcInstruction& insn,
                                std::vector<ExprNode>& stack,
                                bool isConstructorCall);

    // Emit a field access (getfield, getstatic, putfield, putstatic).
    std::string emitFieldAccess(const BcInstruction& insn,
                                 std::vector<ExprNode>& stack,
                                 bool isPut);

    // Emit array creation.
    std::string emitNewArray(const BcInstruction& insn,
                              std::vector<ExprNode>& stack);
};

} // namespace java_emitter
} // namespace retdec

#endif // RETDEC_JAVA_EMITTER_JAVA_EXPR_EMITTER_H
