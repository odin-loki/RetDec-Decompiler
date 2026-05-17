/**
 * @file src/codegen/emitter.cpp
 * @brief C code emitter and CodeGenPass orchestrator.
 *
 * Formats the C AST as a string:
 *   - 4-space indentation per nesting level.
 *   - K&R brace style (`{` on same line, `}` on own line).
 *   - One blank line between top-level functions.
 *   - `#include` directives inferred from known standard functions used.
 *
 * Also contains CodeGenPass::generateFunction, which orchestrates:
 *   1. ExprCoalescer
 *   2. CondNormaliser
 *   3. LoopFormSelector
 *   4. PointerSyntax
 *   5. GotoEliminator
 *   6. Emitter
 */

#include <memory>
#include "retdec/codegen/codegen.h"
#include "retdec/cfg_structure/cfg_structure.h"
#include "retdec/ssa/ssa.h"
#include "retdec/call_conv/call_conv.h"
#include "retdec/dce/dce.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_set>

namespace retdec {
namespace codegen {

// ─── Emitter helpers ──────────────────────────────────────────────────────────

std::string Emitter::ind(int level, int width) const {
    return std::string(static_cast<std::size_t>(level * width), ' ');
}

std::string Emitter::emitType(const CType& type, const std::string& name) const {
    std::string t = type.toString();
    if (!name.empty()) {
        if (type.kind == CType::Kind::Array) {
            std::size_t bracket = t.find('[');
            if (bracket != std::string::npos)
                return t.substr(0, bracket) + name + t.substr(bracket);
        }
        return t + " " + name;
    }
    return t;
}

std::string Emitter::emitExpr(const CExpr& expr, int outerPrec) const {
    return expr.toString(outerPrec);
}

// ─── Statement emitter ────────────────────────────────────────────────────────

std::string Emitter::emitBlock(const CStmt& block, int indent,
                                const Config& cfg) const {
    std::string s = "{\n";
    for (auto& child : block.children) {
        if (child) s += emitStmt(*child, indent + 1, cfg);
    }
    s += ind(indent, cfg.indentWidth) + "}";
    return s;
}

std::string Emitter::emitStmt(const CStmt& stmt, int indent,
                               const Config& cfg) const {
    const std::string I  = ind(indent, cfg.indentWidth);

    switch (stmt.kind) {
    case CStmt::Kind::Block:
        return I + emitBlock(stmt, indent, cfg) + "\n";

    case CStmt::Kind::Decl: {
        std::string s = I + emitType(*stmt.declType, stmt.declName);
        if (stmt.declInit)
            s += " = " + emitExpr(*stmt.declInit);
        return s + ";\n";
    }

    case CStmt::Kind::Assign: {
        std::string lhs = stmt.lhs ? emitExpr(*stmt.lhs) : "?";
        std::string rhs = stmt.expr ? emitExpr(*stmt.expr) : "?";
        return I + lhs + " = " + rhs + ";\n";
    }

    case CStmt::Kind::ExprStmt: {
        if (!stmt.expr) return "";
        return I + emitExpr(*stmt.expr) + ";\n";
    }

    case CStmt::Kind::If: {
        std::string cond = stmt.expr ? emitExpr(*stmt.expr) : "1";
        std::string s = I + "if (" + cond + ") ";
        if (stmt.children.empty()) { return s + "{}\n"; }

        // Then branch — children[0] is a Block.
        s += "{\n";
        const auto* thenBlk = stmt.children[0].get();
        if (thenBlk && thenBlk->kind == CStmt::Kind::Block) {
            for (auto& c : thenBlk->children) if (c) s += emitStmt(*c, indent + 1, cfg);
        } else if (thenBlk) {
            s += emitStmt(*thenBlk, indent + 1, cfg);
        }
        s += I + "}";

        if (stmt.children.size() > 1 && stmt.children[1]) {
            s += " else {\n";
            const auto* elseBlk = stmt.children[1].get();
            if (elseBlk->kind == CStmt::Kind::Block) {
                for (auto& c : elseBlk->children) if (c) s += emitStmt(*c, indent + 1, cfg);
            } else {
                s += emitStmt(*elseBlk, indent + 1, cfg);
            }
            s += I + "}";
        }
        return s + "\n";
    }

    case CStmt::Kind::While: {
        std::string cond = stmt.expr ? emitExpr(*stmt.expr) : "1";
        std::string s = I + "while (" + cond + ") {\n";
        if (!stmt.children.empty() && stmt.children[0]) {
            const auto& body = stmt.children[0];
            if (body->kind == CStmt::Kind::Block)
                for (auto& c : body->children) if (c) s += emitStmt(*c, indent + 1, cfg);
            else
                s += emitStmt(*body, indent + 1, cfg);
        }
        return s + I + "}\n";
    }

    case CStmt::Kind::DoWhile: {
        std::string cond = stmt.expr ? emitExpr(*stmt.expr) : "1";
        std::string s = I + "do {\n";
        if (!stmt.children.empty() && stmt.children[0]) {
            const auto& body = stmt.children[0];
            if (body->kind == CStmt::Kind::Block)
                for (auto& c : body->children) if (c) s += emitStmt(*c, indent + 1, cfg);
            else
                s += emitStmt(*body, indent + 1, cfg);
        }
        return s + I + "} while (" + cond + ");\n";
    }

    case CStmt::Kind::For: {
        std::string initStr = stmt.init ? emitExpr(*stmt.init) : "";
        std::string condStr = stmt.expr ? emitExpr(*stmt.expr) : "";
        std::string incrStr = stmt.incr ? emitExpr(*stmt.incr) : "";
        std::string s = I + "for (" + initStr + "; " + condStr + "; " + incrStr + ") {\n";
        if (!stmt.children.empty() && stmt.children[0]) {
            const auto& body = stmt.children[0];
            if (body->kind == CStmt::Kind::Block)
                for (auto& c : body->children) if (c) s += emitStmt(*c, indent + 1, cfg);
            else
                s += emitStmt(*body, indent + 1, cfg);
        }
        return s + I + "}\n";
    }

    case CStmt::Kind::Switch: {
        std::string cond = stmt.expr ? emitExpr(*stmt.expr) : "0";
        std::string s = I + "switch (" + cond + ") {\n";
        for (auto& c : stmt.children)
            if (c) s += emitStmt(*c, indent, cfg);
        return s + I + "}\n";
    }

    case CStmt::Kind::Case:
        if (stmt.caseValue.has_value())
            return I + "case " + std::to_string(*stmt.caseValue) + ":\n";
        return I + "default:\n";

    case CStmt::Kind::Default:
        return I + "default:\n";

    case CStmt::Kind::Return:
        if (stmt.expr) return I + "return " + emitExpr(*stmt.expr) + ";\n";
        return I + "return;\n";

    case CStmt::Kind::Break:    return I + "break;\n";
    case CStmt::Kind::Continue: return I + "continue;\n";
    case CStmt::Kind::Goto:     return I + "goto " + stmt.label + ";\n";

    case CStmt::Kind::Label:
        return ind(std::max(0, indent - 1), cfg.indentWidth) + stmt.label + ":\n";
    }
    return "";
}

// ─── Function emitter ─────────────────────────────────────────────────────────

std::string Emitter::emitFunction(const CFunction& fn, const Config& cfg) const {
    std::ostringstream out;
    if (fn.isStatic) out << "static ";
    if (fn.isInline) out << "inline ";

    std::string retT = fn.returnType ? fn.returnType->toString() : "void";
    out << retT << " " << fn.name << "(";

    for (std::size_t i = 0; i < fn.params.size(); ++i) {
        if (i) out << ", ";
        out << emitType(*fn.params[i].type, fn.params[i].name);
    }
    if (fn.isVariadic) {
        if (!fn.params.empty()) out << ", ";
        out << "...";
    }
    out << ")\n{\n";

    if (fn.body) {
        for (auto& s : fn.body->children)
            if (s) out << emitStmt(*s, 1, cfg);
    }
    out << "}\n";
    return out.str();
}

// ─── Translation unit emitter ─────────────────────────────────────────────────

static const std::unordered_map<std::string, std::string> kStdIncludes = {
    {"printf",  "stdio.h"},  {"fprintf", "stdio.h"}, {"fopen",  "stdio.h"},
    {"fclose",  "stdio.h"},  {"fread",   "stdio.h"}, {"fwrite", "stdio.h"},
    {"malloc",  "stdlib.h"}, {"calloc",  "stdlib.h"},{"realloc","stdlib.h"},
    {"free",    "stdlib.h"}, {"exit",    "stdlib.h"},{"atoi",   "stdlib.h"},
    {"memcpy",  "string.h"}, {"memset",  "string.h"},{"strcmp", "string.h"},
    {"strlen",  "string.h"}, {"strcpy",  "string.h"},{"strcat", "string.h"},
    {"sin",     "math.h"},   {"cos",     "math.h"},  {"sqrt",   "math.h"},
};

static void collectCalleesExpr(const CExpr* e, std::unordered_set<std::string>& out);
static void collectCalleesStmt(const CStmt* s, std::unordered_set<std::string>& out) {
    if (!s) return;
    if (s->expr)     collectCalleesExpr(s->expr.get(), out);
    if (s->lhs)      collectCalleesExpr(s->lhs.get(), out);
    if (s->init)     collectCalleesExpr(s->init.get(), out);
    if (s->incr)     collectCalleesExpr(s->incr.get(), out);
    if (s->declInit) collectCalleesExpr(s->declInit.get(), out);
    for (auto& c : s->children) collectCalleesStmt(c.get(), out);
}
static void collectCalleesExpr(const CExpr* e, std::unordered_set<std::string>& out) {
    if (!e) return;
    if (e->kind == CExpr::Kind::Call) out.insert(e->callee);
    for (auto& c : e->children) collectCalleesExpr(c.get(), out);
}

std::string Emitter::emitUnit(const CUnit& unit, const Config& cfg) const {
    std::ostringstream out;

    std::set<std::string> includes(unit.includes.begin(), unit.includes.end());
    for (auto& fn : unit.functions) {
        std::unordered_set<std::string> callees;
        if (fn.body) collectCalleesStmt(fn.body.get(), callees);
        for (auto& c : callees) {
            auto it = kStdIncludes.find(c);
            if (it != kStdIncludes.end()) includes.insert(it->second);
        }
    }

    for (auto& inc : includes) out << "#include <" << inc << ">\n";
    if (!includes.empty()) out << "\n";

    for (auto& td : unit.typeDecls) out << td << "\n";
    if (!unit.typeDecls.empty()) out << "\n";

    if (!unit.globalDecls.empty()) out << unit.globalDecls << "\n\n";

    for (std::size_t i = 0; i < unit.functions.size(); ++i) {
        if (i) out << "\n";
        out << emitFunction(unit.functions[i], cfg);
    }
    return out.str();
}

// ─── CodeGenPass ──────────────────────────────────────────────────────────────

namespace {

// Convert condition value ID → CExpr using the coalescer result.
static std::shared_ptr<CExpr> condExpr(uint32_t vid,
                                        const ExprCoalescer::Result& exprs,
                                        const ssa::SSAFunction& fn,
                                        const CondNormaliser& norm) {
    if (vid == UINT32_MAX) return nullptr;
    auto it = exprs.valueExprs.find(vid);
    std::shared_ptr<CExpr> e = (it != exprs.valueExprs.end())
                                ? it->second
                                : CExpr::var("v" + std::to_string(vid), vid);
    return norm.normalise(e, true);
}

// Forward declaration.
static std::shared_ptr<CStmt> buildBody(
        const cfg_structure::StructNode* node,
        const ExprCoalescer::Result& exprs,
        const ssa::SSAFunction& fn,
        const dce::DeadCodeResult& dce,
        const CondNormaliser& condNorm,
        const LoopFormSelector& loopSel,
        const PointerSyntax& ptrSyn,
        CodeGenPass::Stats& stats);

static std::shared_ptr<CStmt> instrToStmt(
        const ssa::IrInstr* instr,
        const ExprCoalescer::Result& exprs,
        const ssa::SSAFunction& fn,
        const PointerSyntax& ptrSyn) {

    using O = ssa::IrInstr::Op;

    auto getUseExpr = [&](std::size_t i) -> std::shared_ptr<CExpr> {
        if (i >= instr->uses.size()) return CExpr::lit("0");
        ssa::ValueId uid = instr->uses[i].valueId;
        auto it = exprs.valueExprs.find(uid);
        return (it != exprs.valueExprs.end())
               ? it->second
               : CExpr::var("v" + std::to_string(uid), uid);
    };

    switch (instr->op) {
    case O::Store: {
        // Store: uses[0] = value, uses[1] = address
        auto addrE = getUseExpr(1);
        auto valE  = getUseExpr(0);
        auto lhsE  = ptrSyn.recover(CExpr::unop(CExpr::UnOpKind::Deref, addrE), {});
        return CStmt::assign(lhsE, valE);
    }
    case O::Ret: {
        if (instr->uses.empty()) return CStmt::retStmt();
        return CStmt::retStmt(getUseExpr(0));
    }
    case O::Call: {
        if (instr->defValue != ssa::kInvalidValue &&
            exprs.inlinedValues.count(instr->defValue))
            return nullptr; // inlined at use site
        auto it = exprs.valueExprs.find(instr->defValue);
        std::shared_ptr<CExpr> callE;
        if (it != exprs.valueExprs.end()) {
            callE = it->second;
        } else {
            std::string fname = instr->calleeName.empty() ? "fn_ptr" : instr->calleeName;
            std::vector<std::shared_ptr<CExpr>> args;
            for (std::size_t i = 0; i < instr->uses.size(); ++i)
                args.push_back(getUseExpr(i));
            callE = CExpr::call(fname, std::move(args));
        }
        if (instr->defValue == ssa::kInvalidValue)
            return CStmt::exprStmt(callE);
        // Assign result to variable.
        const auto* val = fn.value(instr->defValue);
        std::string vname = "v" + std::to_string(instr->defValue);
        if (val && val->varId != ssa::kInvalidVar) {
            const std::string& n = fn.varName(val->varId);
            if (!n.empty()) vname = n + "_" + std::to_string(val->version);
        }
        return CStmt::assign(CExpr::var(vname, instr->defValue), callE);
    }
    default: {
        if (instr->defValue == ssa::kInvalidValue) return nullptr;
        if (exprs.inlinedValues.count(instr->defValue)) return nullptr;
        auto it = exprs.valueExprs.find(instr->defValue);
        if (it == exprs.valueExprs.end()) return nullptr;

        const auto* val = fn.value(instr->defValue);
        std::string vname = "v" + std::to_string(instr->defValue);
        if (val && val->varId != ssa::kInvalidVar) {
            const std::string& n = fn.varName(val->varId);
            if (!n.empty()) vname = n + "_" + std::to_string(val->version);
        }
        return CStmt::assign(CExpr::var(vname, instr->defValue), it->second);
    }
    }
}

static std::shared_ptr<CStmt> buildBody(
        const cfg_structure::StructNode* node,
        const ExprCoalescer::Result& exprs,
        const ssa::SSAFunction& fn,
        const dce::DeadCodeResult& dce,
        const CondNormaliser& condNorm,
        const LoopFormSelector& loopSel,
        const PointerSyntax& ptrSyn,
        CodeGenPass::Stats& stats) {

    if (!node) return CStmt::block();

    using NK = cfg_structure::StructNode::Kind;

    switch (node->kind) {
    case NK::Block: {
        auto blk = CStmt::block();
        const auto* ssaBlk = fn.block(node->blockId);
        if (!ssaBlk) return blk;
        for (const auto* instr : ssaBlk->instrs) {
            if (!instr) continue;
            if (!dce.liveInstrs.empty() && !dce.liveInstrs.count(instr->id))
                continue;
            auto s = instrToStmt(instr, exprs, fn, ptrSyn);
            if (s) blk->children.push_back(s);
        }
        return blk;
    }

    case NK::Sequence: {
        auto seq = CStmt::block();
        for (auto& child : node->children) {
            auto sub = buildBody(child.get(), exprs, fn, dce,
                                  condNorm, loopSel, ptrSyn, stats);
            if (!sub) continue;
            if (sub->kind == CStmt::Kind::Block)
                for (auto& s : sub->children) seq->children.push_back(s);
            else
                seq->children.push_back(sub);
        }
        return seq;
    }

    case NK::IfThen: {
        auto cond = condExpr(node->condValueId, exprs, fn, condNorm);
        if (!cond) cond = CExpr::lit("1");
        auto thenBody = node->children.empty()
                        ? CStmt::block()
                        : buildBody(node->children[0].get(), exprs, fn, dce,
                                    condNorm, loopSel, ptrSyn, stats);
        auto ifS = CStmt::ifStmt(cond);
        ifS->children.push_back(thenBody);
        return ifS;
    }

    case NK::IfThenElse: {
        auto cond = condExpr(node->condValueId, exprs, fn, condNorm);
        if (!cond) cond = CExpr::lit("1");
        auto thenBody = node->children.size() > 0
                        ? buildBody(node->children[0].get(), exprs, fn, dce,
                                    condNorm, loopSel, ptrSyn, stats)
                        : CStmt::block();
        auto elseBody = node->children.size() > 1
                        ? buildBody(node->children[1].get(), exprs, fn, dce,
                                    condNorm, loopSel, ptrSyn, stats)
                        : CStmt::block();
        auto ifS = CStmt::ifStmt(cond);
        ifS->children.push_back(thenBody);
        ifS->children.push_back(elseBody);
        return ifS;
    }

    case NK::While:
    case NK::DoWhile:
    case NK::For:
    case NK::Infinite: {
        auto cond = condExpr(node->condValueId, exprs, fn, condNorm);
        auto body = node->children.empty()
                    ? CStmt::block()
                    : buildBody(node->children[0].get(), exprs, fn, dce,
                                condNorm, loopSel, ptrSyn, stats);
        return loopSel.select(*node, cond, nullptr, nullptr, body);
    }

    case NK::Switch: {
        std::shared_ptr<CExpr> switchE = CExpr::lit("0");
        if (node->condValueId != UINT32_MAX) {
            auto it = exprs.valueExprs.find(node->condValueId);
            if (it != exprs.valueExprs.end()) switchE = it->second;
        }
        auto sw = std::make_shared<CStmt>();
        sw->kind = CStmt::Kind::Switch;
        sw->expr = switchE;
        for (auto& [val, arm] : node->cases) {
            auto cs = std::make_shared<CStmt>();
            cs->kind = CStmt::Kind::Case;
            cs->caseValue = val;
            sw->children.push_back(cs);
            if (arm) {
                auto ab = buildBody(arm.get(), exprs, fn, dce,
                                    condNorm, loopSel, ptrSyn, stats);
                if (ab) {
                    if (ab->kind == CStmt::Kind::Block)
                        for (auto& s : ab->children) sw->children.push_back(s);
                    else
                        sw->children.push_back(ab);
                }
            }
            sw->children.push_back(CStmt::breakStmt());
        }
        if (node->defaultCase) {
            auto dc = std::make_shared<CStmt>();
            dc->kind = CStmt::Kind::Default;
            sw->children.push_back(dc);
            auto db = buildBody(node->defaultCase.get(), exprs, fn, dce,
                                condNorm, loopSel, ptrSyn, stats);
            if (db) {
                if (db->kind == CStmt::Kind::Block)
                    for (auto& s : db->children) sw->children.push_back(s);
                else
                    sw->children.push_back(db);
            }
            sw->children.push_back(CStmt::breakStmt());
        }
        return sw;
    }

    case NK::Goto: {
        // Emit goto target label.
        std::string lbl = node->label;
        if (lbl.empty() && node->gotoTarget != ssa::kInvalidBlock)
            lbl = "L" + std::to_string(node->gotoTarget);
        if (!lbl.empty()) {
            ++stats.gotosRemaining;
            return CStmt::gotoStmt(lbl);
        }
        return CStmt::block();
    }

    default:
        return CStmt::block();
    }
}

} // anonymous namespace

CFunction CodeGenPass::generateFunction(
        const ssa::SSAFunction& fn,
        const cfg_structure::StructNode& structTree,
        const call_conv::CallingConvention& cc,
        const dce::DeadCodeResult& dce,
        const Config& cfg) const {

    CFunction cfn;
    cfn.name = fn.name();

    // Return type.
    using RK = call_conv::RetKind;
    switch (cc.ret.kind) {
    case RK::Void:   cfn.returnType = CType::make(CType::Kind::Void);   break;
    case RK::Float:  cfn.returnType = CType::make(cc.ret.width <= 32
                                                   ? CType::Kind::Float
                                                   : CType::Kind::Double); break;
    case RK::Struct: cfn.returnType = CType::make(CType::Kind::Int64);  break;
    default:
        if (cc.ret.width <= 8)       cfn.returnType = CType::make(CType::Kind::Int8);
        else if (cc.ret.width <= 16) cfn.returnType = CType::make(CType::Kind::Int16);
        else if (cc.ret.width <= 32) cfn.returnType = CType::make(CType::Kind::Int32);
        else                         cfn.returnType = CType::make(CType::Kind::Int64);
        break;
    }

    // Parameters.
    cfn.isVariadic = cc.isVariadic;
    for (std::size_t i = 0; i < cc.args.size(); ++i) {
        CParam p;
        p.name = "arg" + std::to_string(i);
        bool isFp = cc.args[i].isFp;
        uint8_t w = cc.args[i].width;
        if (isFp)        p.type = CType::make(w <= 32 ? CType::Kind::Float
                                                       : CType::Kind::Double);
        else if (w <= 8) p.type = CType::make(CType::Kind::Int8);
        else if (w <=16) p.type = CType::make(CType::Kind::Int16);
        else if (w <=32) p.type = CType::make(CType::Kind::Int32);
        else             p.type = CType::make(CType::Kind::Int64);
        cfn.params.push_back(std::move(p));
    }

    // Phase 1: Expression coalescing.
    ExprCoalescer coalescer;
    auto exprResult = cfg.enableCoalescing ? coalescer.run(fn, dce)
                                           : ExprCoalescer::Result{};
    stats_.coalescedTemps += exprResult.inlinedValues.size();

    // Phase 2: Build condition normaliser, loop selector, pointer syntax.
    CondNormaliser  condNorm;
    LoopFormSelector loopSel;
    PointerSyntax    ptrSyn;

    // Phase 3: Build statement tree from StructNode.
    auto body = buildBody(&structTree, exprResult, fn, dce,
                           condNorm, loopSel, ptrSyn, stats_);
    if (!body || body->kind != CStmt::Kind::Block) {
        auto w = CStmt::block();
        if (body) w->children.push_back(body);
        body = w;
    }

    // Phase 4: Goto elimination.
    if (cfg.enableGotoElim) {
        GotoEliminator ge;
        auto before = stats_.gotosRemaining;
        body = ge.eliminate(body);
        // Recount remaining gotos post-elimination.
        std::unordered_map<std::string, int> remainingGotos;
        auto countGotosNow = [&](auto& self, const CStmt* s) -> void {
            if (!s) return;
            if (s->kind == CStmt::Kind::Goto) ++remainingGotos[s->label];
            for (auto& c : s->children) self(self, c.get());
        };
        countGotosNow(countGotosNow, body.get());
        stats_.gotosEliminated += (stats_.gotosRemaining - before) -
                                   (uint32_t)remainingGotos.size();
        stats_.gotosRemaining = (uint32_t)remainingGotos.size();
    }

    cfn.body = std::move(body);
    ++stats_.totalFunctions;
    return cfn;
}

CUnit CodeGenPass::generateUnit(
        const std::vector<const ssa::SSAFunction*>& fns,
        const std::vector<const cfg_structure::StructNode*>& trees,
        const std::unordered_map<std::string, call_conv::CallingConvention>& ccMap,
        const std::unordered_map<std::string, dce::DeadCodeResult>& dceMap,
        const Config& cfg) const {

    CUnit unit;
    unit.includes = {"stdint.h"};

    for (std::size_t i = 0; i < fns.size(); ++i) {
        if (!fns[i] || i >= trees.size() || !trees[i]) continue;
        const std::string& fname = fns[i]->name();
        const auto& cc  = ccMap.count(fname)  ? ccMap.at(fname)
                                               : call_conv::CallingConvention{};
        const auto& dce = dceMap.count(fname) ? dceMap.at(fname)
                                               : dce::DeadCodeResult{};
        unit.functions.push_back(generateFunction(*fns[i], *trees[i], cc, dce, cfg));
    }
    return unit;
}

std::string CodeGenPass::emit(const CFunction& fn, const Config& cfg) const {
    Emitter e;
    return e.emitFunction(fn, cfg.emitter);
}

} // namespace codegen
} // namespace retdec
