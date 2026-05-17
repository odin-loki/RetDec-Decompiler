/**
 * @file src/csharp_emitter/cs_stmt_emitter.cpp
 * @brief CilStmt → C# statement text.
 */

#include "retdec/csharp_emitter/cs_stmt_emitter.h"

#include <cassert>
#include <variant>

namespace retdec {
namespace csharp_emitter {

// ─── CsStmtEmitter ───────────────────────────────────────────────────────────

CsStmtEmitter::CsStmtEmitter(CsWriter& writer,
                               const CsExprEmitter& expr,
                               Options opts)
    : writer_(writer), expr_(expr), opts_(std::move(opts)) {}

// ─── declType ────────────────────────────────────────────────────────────────

std::string CsStmtEmitter::declType(const BcType& t, const CilExprPtr& rhs) const {
    if (opts_.csharpVersion >= 3) {
        // Use `var` if rhs makes the type obvious
        if (rhs) {
            bool obvious = rhs->isNewobj() || rhs->isNewarr() ||
                           rhs->isConst() || rhs->isLocal() || rhs->isArg();
            if (obvious) return "var";
        }
    }
    return expr_.emitType(t);
}

bool CsStmtEmitter::isVoidCallExpr(const CilExprPtr& e) {
    if (!e) return false;
    if (e->isCall()) return true;
    if (e->isNewobj()) return false;
    return false;
}

// ─── Statement dispatch ───────────────────────────────────────────────────────

void CsStmtEmitter::emitStmt(const CilStmt& s) {
    switch (s.kind) {
    case StmtKind::LocalDecl:        emitLocalDecl(s);        break;
    case StmtKind::Assign:           emitAssign(s);           break;
    case StmtKind::CompoundAssign:   emitCompoundAssign(s);   break;
    case StmtKind::ExprStmt:         emitExprStmt(s);         break;
    case StmtKind::Return:           emitReturn(s);           break;
    case StmtKind::Throw:            emitThrow(s);            break;
    case StmtKind::Rethrow:          emitRethrow(s);          break;
    case StmtKind::If:               emitIf(s);               break;
    case StmtKind::Goto:             emitGoto(s);             break;
    case StmtKind::Label:            emitLabel(s);            break;
    case StmtKind::Leave:            emitLeave(s);            break;
    case StmtKind::Try:              emitTryCatch(s);         break;
    case StmtKind::ForEach:          emitForEach(s);          break;
    case StmtKind::Using:            emitUsing(s);            break;
    case StmtKind::Lock:             emitLock(s);             break;
    case StmtKind::YieldReturn:      emitYieldReturn(s);      break;
    case StmtKind::YieldBreak:       emitYieldBreak(s);       break;
    case StmtKind::Switch:           emitSwitch(s);           break;
    case StmtKind::Fixed:            emitFixed(s);            break;
    case StmtKind::Stackalloc:       emitStackalloc(s);       break;
    case StmtKind::EndFinally:       emitEndFinally(s);       break;
    case StmtKind::EndFilter:        /* suppress internal */  break;
    case StmtKind::AwaitExpr:        emitExprStmt(s);         break;
    // Structured EH sub-kinds (handled by emitTryCatch)
    case StmtKind::Catch:
    case StmtKind::Filter:
    case StmtKind::Finally:
    case StmtKind::Fault:
        break;
    default:
        break;
    }
}

void CsStmtEmitter::emitBody(const std::vector<CilStmt>& stmts) {
    for (const auto& s : stmts)
        emitStmt(s);
}

// ─── Block helpers ────────────────────────────────────────────────────────────

void CsStmtEmitter::emitBlock(const std::vector<CilStmt>& body) {
    if (!opts_.emitBraces && body.size() == 1 && body[0].kind != StmtKind::LocalDecl) {
        writer_.indent();
        emitStmt(body[0]);
        writer_.dedent();
    } else {
        emitForcedBlock(body);
    }
}

void CsStmtEmitter::emitForcedBlock(const std::vector<CilStmt>& body) {
    auto g = writer_.block();
    emitBody(body);
}

// ─── LocalDecl ────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitLocalDecl(const CilStmt& s) {
    std::string typeName = declType(s.declType, s.expr);
    std::string name;
    if (s.target) name = expr_.emit(s.target);
    else          name = "_";

    if (s.expr) {
        writer_.line(typeName + " " + name + " = " + expr_.emit(s.expr) + ";");
    } else {
        writer_.line(typeName + " " + name + ";");
    }
}

// ─── Assign ───────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitAssign(const CilStmt& s) {
    std::string lhs = s.target ? expr_.emit(s.target) : "_";
    std::string rhs = s.expr   ? expr_.emit(s.expr)   : "null";
    writer_.line(lhs + " = " + rhs + ";");
}

void CsStmtEmitter::emitCompoundAssign(const CilStmt& s) {
    // Currently maps to Assign — emitter would need op info for +=, etc.
    emitAssign(s);
}

// ─── ExprStmt ─────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitExprStmt(const CilStmt& s) {
    if (!s.expr) return;
    std::string text = expr_.emit(s.expr);
    if (!text.empty() && text != "null")
        writer_.line(text + ";");
}

// ─── Return ───────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitReturn(const CilStmt& s) {
    if (s.expr) {
        writer_.line("return " + expr_.emit(s.expr) + ";");
    } else {
        writer_.line("return;");
    }
}

// ─── Throw / Rethrow ─────────────────────────────────────────────────────────

void CsStmtEmitter::emitThrow(const CilStmt& s) {
    if (s.expr)
        writer_.line("throw " + expr_.emit(s.expr) + ";");
    else
        writer_.line("throw;");
}

void CsStmtEmitter::emitRethrow(const CilStmt&) {
    writer_.line("throw;");
}

// ─── If ──────────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitIf(const CilStmt& s) {
    std::string cond = s.expr ? expr_.emit(s.expr) : "true";
    writer_.line("if (" + cond + ")");

    if (!s.tryBody.empty()) {
        emitBlock(s.tryBody);   // then-body
        if (!s.catches.empty() && !s.catches[0].body.empty()) {
            writer_.line("else");
            emitBlock(s.catches[0].body);
        }
    } else {
        // If without a body: emit goto
        writer_.indent();
        writer_.line("goto L" + std::to_string(s.blockRef) + ";");
        writer_.dedent();
    }
}

// ─── Goto / Label / Leave ────────────────────────────────────────────────────

void CsStmtEmitter::emitGoto(const CilStmt& s) {
    writer_.line("goto " + s.labelName + ";");
}

void CsStmtEmitter::emitLabel(const CilStmt& s) {
    // Labels use -1 indentation
    writer_.dedent();
    writer_.line(s.labelName + ":");
    writer_.indent();
}

void CsStmtEmitter::emitLeave(const CilStmt& s) {
    // CIL leave is a structured exit from try/catch; translate to goto
    if (!s.labelName.empty())
        writer_.line("goto " + s.labelName + ";");
}

// ─── Try / Catch / Finally ───────────────────────────────────────────────────

void CsStmtEmitter::emitTryCatch(const CilStmt& s) {
    writer_.line("try");
    emitForcedBlock(s.tryBody);

    for (const auto& cc : s.catches)
        emitCatch(cc);

    if (!s.finallyBody.empty()) {
        writer_.line("finally");
        emitForcedBlock(s.finallyBody);
    }

    if (!s.faultBody.empty()) {
        // C# doesn't have `fault`; emit as catch(Exception) + rethrow
        writer_.line("catch (Exception)");
        auto g = writer_.block();
        emitBody(s.faultBody);
        writer_.line("throw;");
    }
}

void CsStmtEmitter::emitCatch(const CilStmt::CatchClause& cc) {
    if (cc.isFilter && cc.filterExpr) {
        std::string filter = expr_.emit(cc.filterExpr);
        if (!cc.varName.empty()) {
            writer_.line("catch (Exception " + CsWriter::safeName(cc.varName) +
                         ") when (" + filter + ")");
        } else {
            writer_.line("catch when (" + filter + ")");
        }
    } else {
        std::string typeName = expr_.emitType(cc.catchType);
        if (typeName == "object" || typeName == "System.Object" || typeName.empty())
            typeName = "Exception";
        if (!cc.varName.empty()) {
            writer_.line("catch (" + typeName + " " + CsWriter::safeName(cc.varName) + ")");
        } else {
            writer_.line("catch (" + typeName + ")");
        }
    }
    emitForcedBlock(cc.body);
}

// ─── ForEach ─────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitForEach(const CilStmt& s) {
    std::string varDecl;
    if (opts_.useVarInForEach) {
        varDecl = "var " + CsWriter::safeName(s.iterVarName);
    } else {
        varDecl = expr_.emitType(s.iterVarType) + " " + CsWriter::safeName(s.iterVarName);
    }
    std::string collection = s.expr ? expr_.emit(s.expr) : "null";
    writer_.line("foreach (" + varDecl + " in " + collection + ")");
    emitForcedBlock(s.loopBody);
}

// ─── Using ───────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitUsing(const CilStmt& s) {
    if (opts_.csharpVersion >= 8 && s.loopBody.empty()) {
        // Using declaration form (C# 8): `using var x = ...;`
        std::string init = s.expr ? expr_.emit(s.expr) : "null";
        writer_.line("using var " + CsWriter::safeName(s.iterVarName) + " = " + init + ";");
        return;
    }
    std::string init = s.expr ? expr_.emit(s.expr) : "null";
    if (!s.iterVarName.empty()) {
        writer_.line("using (var " + CsWriter::safeName(s.iterVarName) + " = " + init + ")");
    } else {
        writer_.line("using (" + init + ")");
    }
    emitForcedBlock(s.loopBody);
}

// ─── Lock ────────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitLock(const CilStmt& s) {
    std::string obj = s.expr ? expr_.emit(s.expr) : "this";
    writer_.line("lock (" + obj + ")");
    emitForcedBlock(s.loopBody);
}

// ─── Yield ───────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitYieldReturn(const CilStmt& s) {
    if (s.expr)
        writer_.line("yield return " + expr_.emit(s.expr) + ";");
    else
        writer_.line("yield return default;");
}

void CsStmtEmitter::emitYieldBreak(const CilStmt&) {
    writer_.line("yield break;");
}

// ─── Switch ───────────────────────────────────────────────────────────────────

void CsStmtEmitter::emitSwitch(const CilStmt& s) {
    std::string subject = s.expr ? expr_.emit(s.expr) : "0";
    writer_.line("switch (" + subject + ")");
    {
        auto g = writer_.block();
        for (const auto& c : s.cases) {
            if (c.values.empty()) {
                writer_.line("default:");
            } else {
                for (const auto& v : c.values) {
                    writer_.line("case " + expr_.emit(v) + ":");
                }
            }
            {
                writer_.indent();
                emitBody(c.body);
                // Add break if body doesn't end with break/goto/return
                bool needsBreak = c.body.empty() ||
                    (c.body.back().kind != StmtKind::Return &&
                     c.body.back().kind != StmtKind::Goto   &&
                     c.body.back().kind != StmtKind::Throw  &&
                     c.body.back().kind != StmtKind::Rethrow);
                if (needsBreak) writer_.line("break;");
                writer_.dedent();
            }
        }
    }
}

// ─── Fixed / Stackalloc ───────────────────────────────────────────────────────

void CsStmtEmitter::emitFixed(const CilStmt& s) {
    std::string typeName = expr_.emitType(s.iterVarType);
    std::string init     = s.expr ? expr_.emit(s.expr) : "null";
    writer_.line("fixed (" + typeName + "* " + CsWriter::safeName(s.iterVarName) + " = " + init + ")");
    emitForcedBlock(s.loopBody);
}

void CsStmtEmitter::emitStackalloc(const CilStmt& s) {
    if (!s.expr) return;
    std::string rhs  = expr_.emit(s.expr);
    std::string name = s.target ? expr_.emit(s.target) : "_";
    writer_.line("var " + name + " = " + rhs + ";");
}

// ─── EndFinally ──────────────────────────────────────────────────────────────

void CsStmtEmitter::emitEndFinally(const CilStmt&) {
    // In structured C# output this is implicit; suppress
}

} // namespace csharp_emitter
} // namespace retdec
