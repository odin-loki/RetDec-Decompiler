/**
 * @file src/py_emitter/py_stmt_emitter.cpp
 */

#include "retdec/py_emitter/py_stmt_emitter.h"

#include <sstream>

namespace retdec {
namespace py_emitter {

PyStmtEmitter::PyStmtEmitter(PyWriter& writer, const PyExprEmitter& expr,
                               Options opts)
    : writer_(writer), expr_(expr), opts_(std::move(opts)) {}

// ─── emitBody ────────────────────────────────────────────────────────────────

void PyStmtEmitter::emitBody(const StmtList& stmts) {
    for (const auto& s : stmts) {
        if (s) emitStmt(*s);
    }
}

void PyStmtEmitter::emitBlock(const StmtList& body) {
    writer_.indent();
    if (body.empty()) {
        writer_.line("pass");
    } else {
        for (const auto& s : body) {
            if (s) emitStmt(*s);
        }
    }
    writer_.dedent();
}

// ─── emitStmt ────────────────────────────────────────────────────────────────

void PyStmtEmitter::emitStmt(const PyStmt& s) {
    switch (s.kind) {
    case PyStmt::Kind::Expr:
        if (s.expr) writer_.line(expr_.emit(s.expr));
        break;
    case PyStmt::Kind::Assign:       emitAssign(s); break;
    case PyStmt::Kind::AugAssign:    emitAugAssign(s); break;
    case PyStmt::Kind::AnnAssign:    emitAnnAssign(s); break;
    case PyStmt::Kind::Delete:       emitDelete(s); break;
    case PyStmt::Kind::Return:       emitReturn(s); break;
    case PyStmt::Kind::Raise:        emitRaise(s); break;
    case PyStmt::Kind::Assert:       emitAssert(s); break;
    case PyStmt::Kind::Pass:         writer_.line("pass"); break;
    case PyStmt::Kind::Break:        writer_.line("break"); break;
    case PyStmt::Kind::Continue:     writer_.line("continue"); break;
    case PyStmt::Kind::Global:       emitGlobal(s); break;
    case PyStmt::Kind::Nonlocal:     emitNonlocal(s); break;
    case PyStmt::Kind::Import:       emitImport(s); break;
    case PyStmt::Kind::ImportFrom:   emitImportFrom(s); break;
    case PyStmt::Kind::If:           emitIf(s); break;
    case PyStmt::Kind::For:          emitFor(s, false); break;
    case PyStmt::Kind::AsyncFor:     emitFor(s, true); break;
    case PyStmt::Kind::While:        emitWhile(s); break;
    case PyStmt::Kind::With:         emitWith(s, false); break;
    case PyStmt::Kind::AsyncWith:    emitWith(s, true); break;
    case PyStmt::Kind::FunctionDef:  emitFunctionDef(s, false); break;
    case PyStmt::Kind::AsyncFunctionDef: emitFunctionDef(s, true); break;
    case PyStmt::Kind::ClassDef:     emitClassDef(s); break;
    case PyStmt::Kind::Try:          emitTry(s); break;
    case PyStmt::Kind::TryStar:      emitTryStar(s); break;
    case PyStmt::Kind::Match:        emitMatch(s); break;
    case PyStmt::Kind::Yield: {
        // yield as statement (wraps yield expr)
        if (s.expr) writer_.line(expr_.emit(s.expr));
        break;
    }
    }
}

// ─── Simple statements ────────────────────────────────────────────────────────

void PyStmtEmitter::emitAssign(const PyStmt& s) {
    std::string lhs = expr_.emitList(s.targets, " = ");
    std::string rhs = s.expr ? expr_.emit(s.expr) : "None";
    writer_.line(lhs + " = " + rhs);
}

void PyStmtEmitter::emitAugAssign(const PyStmt& s) {
    std::string tgt = s.targets.empty() ? "?" : expr_.emit(s.targets[0]);
    std::string val = s.expr ? expr_.emit(s.expr) : "?";
    writer_.line(tgt + " " + augOpStr(s.augOp) + "= " + val);
}

void PyStmtEmitter::emitAnnAssign(const PyStmt& s) {
    std::string tgt = s.targets.empty() ? "?" : expr_.emit(s.targets[0]);
    std::string ann = expr_.emitAnnotation(s.expr3);
    if (s.expr2) {
        writer_.line(tgt + ": " + ann + " = " + expr_.emit(s.expr2));
    } else {
        writer_.line(tgt + ": " + ann);
    }
}

void PyStmtEmitter::emitDelete(const PyStmt& s) {
    if (!s.targets.empty())
        writer_.line("del " + expr_.emitList(s.targets));
    else if (s.expr)
        writer_.line("del " + expr_.emit(s.expr));
    else
        writer_.line("del ?");
}

void PyStmtEmitter::emitReturn(const PyStmt& s) {
    if (s.expr)
        writer_.line("return " + expr_.emit(s.expr));
    else
        writer_.line("return");
}

void PyStmtEmitter::emitRaise(const PyStmt& s) {
    if (!s.expr) {
        writer_.line("raise");
    } else if (s.expr2) {
        writer_.line("raise " + expr_.emit(s.expr) + " from " + expr_.emit(s.expr2));
    } else {
        writer_.line("raise " + expr_.emit(s.expr));
    }
}

void PyStmtEmitter::emitAssert(const PyStmt& s) {
    if (s.expr2)
        writer_.line("assert " + expr_.emit(s.expr) + ", " + expr_.emit(s.expr2));
    else
        writer_.line("assert " + expr_.emit(s.expr));
}

void PyStmtEmitter::emitImport(const PyStmt& s) {
    std::ostringstream ss;
    ss << "import ";
    for (size_t i = 0; i < s.aliases.size(); ++i) {
        if (i) ss << ", ";
        ss << s.aliases[i].name;
        if (s.aliases[i].asname) ss << " as " << *s.aliases[i].asname;
    }
    writer_.line(ss.str());
}

void PyStmtEmitter::emitImportFrom(const PyStmt& s) {
    std::ostringstream ss;
    ss << "from ";
    for (int i = 0; i < s.level; ++i) ss << ".";
    ss << (s.module.has_value() ? *s.module : "") << " import ";
    for (size_t i = 0; i < s.aliases.size(); ++i) {
        if (i) ss << ", ";
        ss << s.aliases[i].name;
        if (s.aliases[i].asname) ss << " as " << *s.aliases[i].asname;
    }
    writer_.line(ss.str());
}

void PyStmtEmitter::emitGlobal(const PyStmt& s) {
    std::string ns;
    for (size_t i = 0; i < s.names.size(); ++i) {
        if (i) ns += ", ";
        ns += PyWriter::safeName(s.names[i]);
    }
    writer_.line("global " + ns);
}

void PyStmtEmitter::emitNonlocal(const PyStmt& s) {
    std::string ns;
    for (size_t i = 0; i < s.names.size(); ++i) {
        if (i) ns += ", ";
        ns += PyWriter::safeName(s.names[i]);
    }
    writer_.line("nonlocal " + ns);
}

// ─── Compound statements ──────────────────────────────────────────────────────

void PyStmtEmitter::emitIf(const PyStmt& s) {
    writer_.line("if " + expr_.emit(s.expr) + ":");
    emitBlock(s.body);
    if (!s.orelse.empty()) {
        // Check if orelse is a single 'if' → emit as elif
        if (s.orelse.size() == 1 && s.orelse[0]->kind == PyStmt::Kind::If) {
            // Replace 'else:\n    if' with 'elif'
            const auto& elif = *s.orelse[0];
            writer_.line("elif " + expr_.emit(elif.expr) + ":");
            emitBlock(elif.body);
            if (!elif.orelse.empty()) {
                PyStmt elseStmt;
                elseStmt.kind  = PyStmt::Kind::If;
                elseStmt.orelse= elif.orelse;
                // just emit else block
                writer_.line("else:");
                emitBlock(elif.orelse);
            }
        } else {
            writer_.line("else:");
            emitBlock(s.orelse);
        }
    }
}

void PyStmtEmitter::emitFor(const PyStmt& s, bool isAsync) {
    std::string tgt  = expr_.emit(s.forTarget);
    std::string iter = expr_.emit(s.forIter);
    std::string prefix = isAsync ? "async for " : "for ";
    writer_.line(prefix + tgt + " in " + iter + ":");
    emitBlock(s.body);
    if (!s.orelse.empty()) {
        writer_.line("else:");
        emitBlock(s.orelse);
    }
}

void PyStmtEmitter::emitWhile(const PyStmt& s) {
    writer_.line("while " + expr_.emit(s.expr) + ":");
    emitBlock(s.body);
    if (!s.orelse.empty()) {
        writer_.line("else:");
        emitBlock(s.orelse);
    }
}

void PyStmtEmitter::emitWith(const PyStmt& s, bool isAsync) {
    std::ostringstream ss;
    ss << (isAsync ? "async with " : "with ");
    for (size_t i = 0; i < s.withItems.size(); ++i) {
        if (i) ss << ", ";
        ss << expr_.emit(s.withItems[i].context_expr);
        if (s.withItems[i].optional_vars)
            ss << " as " << expr_.emit(s.withItems[i].optional_vars);
    }
    ss << ":";
    writer_.line(ss.str());
    emitBlock(s.body);
}

void PyStmtEmitter::emitTry(const PyStmt& s) {
    writer_.line("try:");
    emitBlock(s.body);
    for (const auto& h : s.handlers) {
        emitExceptHandler(h);
    }
    if (!s.orelse.empty()) {
        writer_.line("else:");
        emitBlock(s.orelse);
    }
    if (!s.finalbody.empty()) {
        writer_.line("finally:");
        emitBlock(s.finalbody);
    }
}

void PyStmtEmitter::emitTryStar(const PyStmt& s) {
    // Python 3.11+ except*
    writer_.line("try:");
    emitBlock(s.body);
    for (const auto& h : s.handlers) {
        std::ostringstream ss;
        ss << "except* " << (h.type ? expr_.emit(h.type) : "Exception");
        if (h.name) ss << " as " << *h.name;
        ss << ":";
        writer_.line(ss.str());
        emitBlock(h.body);
    }
    if (!s.finalbody.empty()) {
        writer_.line("finally:");
        emitBlock(s.finalbody);
    }
}

void PyStmtEmitter::emitMatch(const PyStmt& s) {
    writer_.line("match " + expr_.emit(s.expr) + ":");
    writer_.indent();
    for (const auto& mc : s.cases) {
        emitMatchCase(mc);
    }
    writer_.dedent();
}

void PyStmtEmitter::emitExceptHandler(const PyExceptHandler& h) {
    std::string hdr = "except";
    if (h.type) {
        hdr += " " + expr_.emit(h.type);
        if (h.name) hdr += " as " + *h.name;
    }
    hdr += ":";
    writer_.line(hdr);
    emitBlock(h.body);
}

void PyStmtEmitter::emitMatchCase(const PyMatchCase& mc) {
    std::ostringstream ss;
    ss << "case ";
    if (mc.pattern) emitPattern(*mc.pattern);
    if (mc.guard) ss << " if " << expr_.emit(mc.guard);
    writer_.line(ss.str() + ":");
    emitBlock(mc.body);
}

void PyStmtEmitter::emitPattern(const PyPattern& pat) {
    // Patterns are complex; emit a placeholder for now
    writer_.write("_");
    (void)pat;
}

// ─── Functions and Classes ───────────────────────────────────────────────────

void PyStmtEmitter::emitDecorators(const ExprList& decorators) {
    for (const auto& d : decorators) {
        if (d) writer_.line("@" + expr_.emit(d));
    }
}

void PyStmtEmitter::emitFunctionDef(const PyStmt& s, bool isAsync) {
    emitDecorators(s.decorators);
    std::string sig = funcSignature(s);
    std::string prefix = isAsync ? "async def " : "def ";
    writer_.line(prefix + sig + ":");
    emitBlock(s.body);
}

std::string PyStmtEmitter::funcSignature(const PyStmt& s) const {
    std::string params = expr_.emitArguments(s.funcArgs, true);
    std::string sig    = PyWriter::safeName(s.funcName) + "(" + params + ")";
    if (s.returnAnnotation)
        sig += " -> " + expr_.emitAnnotation(s.returnAnnotation);
    return sig;
}

void PyStmtEmitter::emitClassDef(const PyStmt& s) {
    emitDecorators(s.classDecorators);
    std::ostringstream ss;
    ss << "class " << PyWriter::safeName(s.className);
    if (!s.bases.empty() || !s.classKeywords.empty()) {
        ss << "(";
        ss << expr_.emitList(s.bases);
        if (!s.bases.empty() && !s.classKeywords.empty()) ss << ", ";
        ss << expr_.emitKeywords(s.classKeywords);
        ss << ")";
    }
    ss << ":";
    writer_.line(ss.str());
    emitBlock(s.body);
}

// ─── augOpStr ────────────────────────────────────────────────────────────────

std::string PyStmtEmitter::augOpStr(AugOp op) const {
    switch (op) {
    case AugOp::Add:      return "+";
    case AugOp::Sub:      return "-";
    case AugOp::Mult:     return "*";
    case AugOp::MatMult:  return "@";
    case AugOp::Div:      return "/";
    case AugOp::FloorDiv: return "//";
    case AugOp::Mod:      return "%";
    case AugOp::Pow:      return "**";
    case AugOp::LShift:   return "<<";
    case AugOp::RShift:   return ">>";
    case AugOp::BitOr:    return "|";
    case AugOp::BitXor:   return "^";
    case AugOp::BitAnd:   return "&";
    }
    return "+";
}

} // namespace py_emitter
} // namespace retdec
