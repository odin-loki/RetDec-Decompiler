/**
 * @file include/retdec/csharp_emitter/cs_stmt_emitter.h
 * @brief CilStmt list → C# statement text.
 *
 * Handles all statement kinds, including structured control flow,
 * exception handling, high-level C# patterns (async/await, yield,
 * using, lock, foreach, pattern matching, switch expressions).
 *
 * ## C# version–gated syntax
 *
 *   v8+:  switch expression, using declaration, null-coalescing assignment (??=)
 *   v9+:  pattern matching (is T { } with deconstruct), records with expression, top-level statements
 *   v10+: file-scoped namespace
 *   v11+: required members, raw string literals, list patterns
 *   v12+: primary constructors, collection expressions ([ ])
 */

#ifndef RETDEC_CSHARP_EMITTER_CS_STMT_EMITTER_H
#define RETDEC_CSHARP_EMITTER_CS_STMT_EMITTER_H

#include "retdec/cil_reconstruct/cil_var_recovery.h"
#include "retdec/csharp_emitter/cs_expr_emitter.h"
#include "retdec/csharp_emitter/cs_writer.h"

#include <string>
#include <vector>

namespace retdec {
namespace csharp_emitter {

using namespace cil_reconstruct;

// ─── CsStmtEmitter ───────────────────────────────────────────────────────────

/**
 * @brief Emits C# statements into a CsWriter.
 *
 * Each `emit*` method writes zero or more lines to the backing CsWriter.
 * Methods are composable — `emitBody` delegates to `emitStmt` for each stmt.
 */
class CsStmtEmitter {
public:
    struct Options {
        bool preferExpressionBodies = true;  ///< `=> expr;` for single-expr methods
        bool useVarInForEach        = true;  ///< `foreach (var x in ...)`
        bool emitBraces             = true;  ///< Always emit braces (even for single stmts)
        bool inlineReturnExpr       = true;  ///< Return x; → single expression body
        int  csharpVersion          = 12;
    };
    static Options defaultOptions() noexcept { return {}; }

    CsStmtEmitter(CsWriter& writer,
                  const CsExprEmitter& expr,
                  Options opts = defaultOptions());

    /// Emit a list of statements (method / block body).
    void emitBody(const std::vector<CilStmt>& stmts);

    /// Emit a single statement.
    void emitStmt(const CilStmt& stmt);

private:
    CsWriter&           writer_;
    const CsExprEmitter& expr_;
    Options              opts_;

    // ── Statement dispatchers ─────────────────────────────────────────────────

    void emitLocalDecl     (const CilStmt& s);
    void emitAssign        (const CilStmt& s);
    void emitCompoundAssign(const CilStmt& s);
    void emitExprStmt      (const CilStmt& s);
    void emitReturn        (const CilStmt& s);
    void emitThrow         (const CilStmt& s);
    void emitRethrow       (const CilStmt& s);
    void emitIf            (const CilStmt& s);
    void emitGoto          (const CilStmt& s);
    void emitLabel         (const CilStmt& s);
    void emitLeave         (const CilStmt& s);
    void emitTryCatch      (const CilStmt& s);
    void emitForEach       (const CilStmt& s);
    void emitUsing         (const CilStmt& s);
    void emitLock          (const CilStmt& s);
    void emitYieldReturn   (const CilStmt& s);
    void emitYieldBreak    (const CilStmt& s);
    void emitSwitch        (const CilStmt& s);
    void emitFixed         (const CilStmt& s);
    void emitStackalloc    (const CilStmt& s);
    void emitEndFinally    (const CilStmt& s);

    // ── Helpers ───────────────────────────────────────────────────────────────

    /// Emit a block body with braces (or without for single-stmt if !opts_.emitBraces).
    void emitBlock(const std::vector<CilStmt>& body);

    /// Emit a brace-enclosed block even for single-statement bodies.
    void emitForcedBlock(const std::vector<CilStmt>& body);

    /// Emit a catch clause.
    void emitCatch(const CilStmt::CatchClause& cc);

    /// Format a type name for local declarations (use `var` where possible).
    std::string declType(const BcType& t, const CilExprPtr& rhs) const;

    /// True if expression is a simple primary (no side effects that need stmt form).
    static bool isVoidCallExpr(const CilExprPtr& e);
};

} // namespace csharp_emitter
} // namespace retdec

#endif // RETDEC_CSHARP_EMITTER_CS_STMT_EMITTER_H
