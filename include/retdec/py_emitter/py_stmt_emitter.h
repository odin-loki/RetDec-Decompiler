/**
 * @file include/retdec/py_emitter/py_stmt_emitter.h
 * @brief Python statement emitter: PyStmt list → Python source.
 *
 * Handles all statement kinds including:
 *   - Simple: assign, augassign, annassign, expr, delete, return,
 *     raise, assert, pass, break, continue, global, nonlocal,
 *     import, import_from
 *   - Compound: if/elif/else, for/else, while/else, with (async),
 *     try/except/else/finally, try/except* (3.11+), match/case,
 *     def/async def, class
 * Version-gated syntax emitted only for the target Python version.
 */

#ifndef RETDEC_PY_EMITTER_PY_STMT_EMITTER_H
#define RETDEC_PY_EMITTER_PY_STMT_EMITTER_H

#include "retdec/py_emitter/py_expr_emitter.h"
#include "retdec/py_emitter/py_writer.h"
#include "retdec/py_reconstruct/py_ast_nodes.h"

namespace retdec {
namespace py_emitter {

class PyStmtEmitter {
public:
    struct Options {
        int pyMajor = 3;
        int pyMinor = 10;
    };
    static Options defaultOptions() noexcept { return {}; }

    PyStmtEmitter(PyWriter& writer, const PyExprEmitter& expr,
                  Options opts = defaultOptions());

    void emitBody(const StmtList& stmts);
    void emitStmt(const PyStmt& stmt);

private:
    PyWriter&            writer_;
    const PyExprEmitter& expr_;
    Options              opts_;

    bool atLeast(int maj, int min) const {
        return opts_.pyMajor > maj ||
               (opts_.pyMajor == maj && opts_.pyMinor >= min);
    }

    void emitBlock(const StmtList& body);

    void emitAssign(const PyStmt& s);
    void emitAugAssign(const PyStmt& s);
    void emitAnnAssign(const PyStmt& s);
    void emitDelete(const PyStmt& s);
    void emitReturn(const PyStmt& s);
    void emitRaise(const PyStmt& s);
    void emitAssert(const PyStmt& s);
    void emitImport(const PyStmt& s);
    void emitImportFrom(const PyStmt& s);
    void emitGlobal(const PyStmt& s);
    void emitNonlocal(const PyStmt& s);
    void emitIf(const PyStmt& s);
    void emitFor(const PyStmt& s, bool isAsync);
    void emitWhile(const PyStmt& s);
    void emitWith(const PyStmt& s, bool isAsync);
    void emitTry(const PyStmt& s);
    void emitTryStar(const PyStmt& s);
    void emitMatch(const PyStmt& s);
    void emitFunctionDef(const PyStmt& s, bool isAsync);
    void emitClassDef(const PyStmt& s);

    void emitDecorators(const ExprList& decorators);
    void emitExceptHandler(const PyExceptHandler& h);
    void emitMatchCase(const PyMatchCase& mc);
    void emitPattern(const PyPattern& pat);

    std::string augOpStr(AugOp op) const;
    std::string funcSignature(const PyStmt& s) const;
};

} // namespace py_emitter
} // namespace retdec

#endif // RETDEC_PY_EMITTER_PY_STMT_EMITTER_H
