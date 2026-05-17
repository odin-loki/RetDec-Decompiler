/**
 * @file include/retdec/py_emitter/py_expr_emitter.h
 * @brief Python expression emitter: PyExpr tree → Python source string.
 */

#ifndef RETDEC_PY_EMITTER_PY_EXPR_EMITTER_H
#define RETDEC_PY_EMITTER_PY_EXPR_EMITTER_H

#include "retdec/py_emitter/py_writer.h"
#include "retdec/py_reconstruct/py_ast_nodes.h"

#include <string>

namespace retdec {
namespace py_emitter {

using namespace py_reconstruct;

class PyExprEmitter {
public:
    explicit PyExprEmitter(PyWriter& writer);

    /**
     * @brief Emit a single expression as a string.
     * @param expr       The expression to emit.
     * @param parentPrec Precedence of the parent context (for parenthesisation).
     * @return Source text of the expression.
     */
    std::string emit(const PyExprPtr& expr, PyPrec parentPrec = PyPrec::None_) const;

    /// Emit a comma-separated list of expressions.
    std::string emitList(const ExprList& exprs, const std::string& sep = ", ") const;

    /// Emit a type annotation expression (same as emit but contextually cleaner).
    std::string emitAnnotation(const PyExprPtr& ann) const;

    /// Emit keyword argument list.
    std::string emitKeywords(const std::vector<PyKeyword>& kws) const;

    /// Emit function argument list (def signature).
    std::string emitArguments(const PyArguments& args, bool inDef) const;

private:
    PyWriter& writer_;

    std::string emitConst(const PyExpr& e) const;
    std::string emitBinOp(const PyExpr& e, PyPrec parentPrec) const;
    std::string emitUnaryOp(const PyExpr& e, PyPrec parentPrec) const;
    std::string emitBoolOp(const PyExpr& e, PyPrec parentPrec) const;
    std::string emitCompare(const PyExpr& e, PyPrec parentPrec) const;
    std::string emitCall(const PyExpr& e) const;
    std::string emitAttr(const PyExpr& e) const;
    std::string emitSubscript(const PyExpr& e) const;
    std::string emitStarred(const PyExpr& e) const;
    std::string emitIfExp(const PyExpr& e, PyPrec parentPrec) const;
    std::string emitLambda(const PyExpr& e) const;
    std::string emitJoinedStr(const PyExpr& e) const;
    std::string emitFormattedValue(const PyExpr& e) const;
    std::string emitNamedExpr(const PyExpr& e) const;
    std::string emitComp(const PyExpr& e) const;
    std::string emitYield(const PyExpr& e) const;
    std::string emitAwait(const PyExpr& e) const;

    static std::string binOpStr(BinOp op);
    static std::string unOpStr(UnaryOp op);
    static std::string cmpOpStr(CmpOp op);
    static PyPrec      binOpPrec(BinOp op);
    static std::string parenIf(const std::string& s, PyPrec myPrec, PyPrec parentPrec);
};

} // namespace py_emitter
} // namespace retdec

#endif // RETDEC_PY_EMITTER_PY_EXPR_EMITTER_H
