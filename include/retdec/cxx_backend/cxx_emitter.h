/**
 * @file include/retdec/cxx_backend/cxx_emitter.h
 * @brief C++ source emitter — formats a CxxUnit into a .cpp or .c string.
 *
 * Extends the plain-C `codegen::Emitter` to handle:
 *   - Class / struct declarations (with access specifiers)
 *   - Constructor / destructor / method bodies
 *   - Virtual dispatch, override, final, pure-virtual
 *   - Template declarations
 *   - Namespace wrappers (including anonymous namespaces)
 *   - `new`/`delete` expressions
 *   - `try`/`catch` blocks
 *   - `throw` expressions
 *   - Named C++ casts
 *   - `using` declarations and `using namespace`
 *   - `enum class`
 *   - Reference types (&, &&)
 *   - Lambda expressions (best-effort)
 *
 * When `CxxUnit::isCxx == false`, falls back to the plain-C `codegen::Emitter`.
 */

#ifndef RETDEC_CXX_BACKEND_CXX_EMITTER_H
#define RETDEC_CXX_BACKEND_CXX_EMITTER_H

#include "retdec/cxx_backend/cxx_ast.h"
#include "retdec/codegen/codegen.h"

#include <string>

namespace retdec {
namespace cxx_backend {

struct CxxEmitConfig {
    int  indentWidth     = 4;
    bool krBraces        = true;     ///< K&R: { on same line
    bool addLineComments = false;    ///< /* addr: 0xXXXX */
    bool declsAtTop      = true;     ///< hoist locals to function top
    bool emitFileHeader  = true;     ///< opening comment
    bool groupAccessSpec = true;     ///< emit public:/private: groupings
    bool emitOverride    = true;     ///< emit override keyword
};

class CxxEmitter {
public:
    explicit CxxEmitter(CxxEmitConfig cfg = CxxEmitConfig{});

    /// Emit the whole translation unit.
    std::string emitUnit(const CxxUnit& unit) const;

    // ── Individual node emitters (public for testing) ─────────────────────────

    std::string emitClass(const CxxClass& cls, int indent) const;
    std::string emitMethod(const CxxMethod& m, int indent,
                           const std::string& className = "") const;
    std::string emitTemplate(const CxxTemplate& tmpl) const;
    std::string emitEnum(const CxxEnum& en, int indent) const;
    std::string emitNamespace(const CxxNamespace& ns, int indent) const;
    std::string emitUsing(const CxxUsing& u) const;
    std::string emitTry(const CxxTryStmt& t, int indent) const;
    std::string emitNew(const CxxNewExpr& n) const;
    std::string emitDelete(const CxxDeleteExpr& d) const;
    std::string emitCxxCast(const CxxCastExpr& c) const;
    std::string emitThrow(const CxxThrowExpr& t) const;
    std::string emitScope(const CxxScopeExpr& s) const;
    std::string emitMethodCall(const CxxMethodCallExpr& mc) const;
    std::string emitLambda(const CxxLambdaExpr& lam, int indent) const;

    // ── Type / expression printing using parent codegen::Emitter ──────────────

    std::string emitType(const CType& t, const std::string& name = "") const;
    std::string emitExpr(const CExpr& e, int outerPrec = 0) const;
    std::string emitStmt(const CStmt& s, int indent) const;
    std::string emitFunction(const codegen::CFunction& fn, int indent = 0) const;

private:
    CxxEmitConfig   cfg_;
    codegen::Emitter plain_;  ///< delegate for plain-C nodes

    std::string ind(int level) const;
    std::string openBrace(int indent, bool sameLine) const;
    std::string closeBrace(int indent) const;
    std::string emitFieldList(const std::vector<CxxClass::Field>& fields,
                               const std::string& access, int indent) const;
    std::string emitMethodList(const std::vector<CxxMethod>& methods,
                                const std::string& access, int indent,
                                const std::string& className) const;
    std::string emitIncludes(const std::vector<std::string>& system,
                              const std::vector<std::string>& user) const;
};

} // namespace cxx_backend
} // namespace retdec

#endif // RETDEC_CXX_BACKEND_CXX_EMITTER_H
