/**
 * @file src/cil_reconstruct/cil_patterns.cpp
 * @brief CIL high-level pattern detection.
 */

#include <memory>
#include "retdec/cil_reconstruct/cil_patterns.h"

#include <algorithm>
#include <cassert>
#include <functional>

namespace retdec {
namespace cil_reconstruct {

// ─── CilPatternDetector ───────────────────────────────────────────────────────

CilPatternDetector::CilPatternDetector(const Options& opts) : opts_(opts) {}

// ─── walkStmts ───────────────────────────────────────────────────────────────

void CilPatternDetector::walkStmts(std::vector<CilStmt>& stmts,
                                    const StmtRewriter& rewriter) {
    for (size_t i = 0; i < stmts.size(); ) {
        bool advanced = rewriter(stmts[i], stmts);
        if (!advanced) {
            // Walk into sub-bodies
            walkStmts(stmts[i].tryBody, rewriter);
            for (auto& c : stmts[i].catches) walkStmts(c.body, rewriter);
            walkStmts(stmts[i].finallyBody, rewriter);
            walkStmts(stmts[i].faultBody, rewriter);
            walkStmts(stmts[i].loopBody, rewriter);
            for (auto& c : stmts[i].cases) walkStmts(c.body, rewriter);
            ++i;
        }
    }
}

// ─── matchPropertyCall ───────────────────────────────────────────────────────

std::optional<PropertyPattern> CilPatternDetector::matchPropertyCall(
        const CilStmt& stmt) {
    if (stmt.kind != StmtKind::ExprStmt && stmt.kind != StmtKind::Assign)
        return std::nullopt;

    const CilExprPtr& e = (stmt.kind == StmtKind::ExprStmt) ? stmt.expr
                                                              : stmt.expr;
    if (!e || !e->isCall()) return std::nullopt;

    const ExprCall& call = e->asCall();
    const std::string& mname = call.methodName;

    auto startsWithPrefix = [&](const std::string& prefix) {
        return mname.size() > prefix.size() && mname.substr(0, prefix.size()) == prefix;
    };

    PropertyPattern pp;
    pp.ownerType = call.className;

    if (startsWithPrefix("get_")) {
        pp.kind = PropertyPattern::Getter;
        pp.propertyName = mname.substr(4);
        pp.isStatic = (call.obj == nullptr);
        return pp;
    }
    if (startsWithPrefix("set_")) {
        pp.kind = PropertyPattern::Setter;
        pp.propertyName = mname.substr(4);
        pp.isStatic = (call.obj == nullptr);
        if (!call.args.empty()) pp.valueExpr = call.args.back();
        return pp;
    }
    if (startsWithPrefix("add_")) {
        pp.kind = PropertyPattern::EventAdd;
        pp.propertyName = mname.substr(4);
        return pp;
    }
    if (startsWithPrefix("remove_")) {
        pp.kind = PropertyPattern::EventRemove;
        pp.propertyName = mname.substr(7);
        return pp;
    }
    return std::nullopt;
}

// ─── matchLambdaConstruction ─────────────────────────────────────────────────

std::optional<LambdaInfo> CilPatternDetector::matchLambdaConstruction(
        const CilExprPtr& expr) {
    if (!expr) return std::nullopt;

    // Pattern: newobj Func<>/Action<> with a ldftn as the method argument
    if (!expr->isNewobj()) return std::nullopt;

    const ExprNewobj& no = expr->asNewobj();
    // Check if the delegate type looks like a Func/Action/Predicate
    const std::string& dt = no.className;
    bool isDelegate = (dt.find("Func") != std::string::npos ||
                       dt.find("Action") != std::string::npos ||
                       dt.find("Predicate") != std::string::npos ||
                       dt.find("Comparison") != std::string::npos ||
                       dt.find("EventHandler") != std::string::npos);
    if (!isDelegate) return std::nullopt;

    // Look for ldftn or ldvirtftn argument
    for (const auto& arg : no.args) {
        if (!arg) continue;
        if (const auto* lf = std::get_if<ExprLdFtn>(&arg->data)) {
            LambdaInfo li;
            li.targetMethod = lf->methodName;
            li.delegateType = dt;
            // Check for closure pattern: <ClassName>c.<>9__N_M
            if (lf->methodName.find("<>b__") != std::string::npos ||
                lf->methodName.find("b__") != std::string::npos) {
                li.captureClass = lf->className;
            }
            return li;
        }
    }
    return std::nullopt;
}

// ─── isAsyncSMClass / isIteratorSMClass ──────────────────────────────────────

bool CilPatternDetector::isAsyncSMClass(const BcClass& cls) {
    // Compiler-generated: name like <MethodName>d__N, implements IAsyncStateMachine
    if (cls.name.find("d__") == std::string::npos &&
        cls.name.find(">d__") == std::string::npos) return false;
    for (const auto& iface : cls.interfaces) {
        std::string ifaceStr = iface.toString();
        if (ifaceStr.find("IAsyncStateMachine") != std::string::npos) return true;
    }
    return false;
}

bool CilPatternDetector::isIteratorSMClass(const BcClass& cls) {
    if (cls.name.find("d__") == std::string::npos &&
        cls.name.find(">d__") == std::string::npos) return false;
    for (const auto& iface : cls.interfaces) {
        std::string ifaceStr = iface.toString();
        if (ifaceStr.find("IEnumerator") != std::string::npos) return true;
    }
    return false;
}

// ─── matchIsTypePattern ───────────────────────────────────────────────────────

std::optional<IsTypePattern> CilPatternDetector::matchIsTypePattern(
        const std::vector<CilStmt>& stmts, size_t pos) {
    // Pattern: isinst T expr (in ExprStmt) followed by brfalse + unbox.any + stloc
    // This is a simplified detection; full implementation would trace through
    // CFG edges.
    if (pos >= stmts.size()) return std::nullopt;

    const CilStmt& stmt = stmts[pos];
    if (stmt.kind != StmtKind::ExprStmt || !stmt.expr) return std::nullopt;
    if (!stmt.expr->isCall() && !std::holds_alternative<ExprIsinst>(stmt.expr->data))
        return std::nullopt;

    if (const auto* ii = std::get_if<ExprIsinst>(&stmt.expr->data)) {
        IsTypePattern p;
        p.subject     = ii->expr;
        p.testedType  = ii->targetType;
        p.boundVarName = "when"; // placeholder
        return p;
    }
    return std::nullopt;
}

// ─── matchUsingPattern ───────────────────────────────────────────────────────

bool CilPatternDetector::matchUsingPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        std::string& varName, CilExprPtr& initExpr, std::vector<CilStmt>& body) {
    if (pos >= stmts.size()) return false;
    const CilStmt& s = stmts[pos];

    // Pattern: try { ... } finally { local.Dispose() }
    if (s.kind != StmtKind::Try) return false;
    if (s.finallyBody.empty()) return false;

    // Check finally body for Dispose() call
    for (const auto& fs : s.finallyBody) {
        if (fs.kind != StmtKind::ExprStmt || !fs.expr) continue;
        if (!fs.expr->isCall()) continue;
        const ExprCall& call = fs.expr->asCall();
        if (call.methodName == "Dispose" || call.methodName == "DisposeAsync") {
            // Found the using pattern
            body = s.tryBody;
            // Try to find the preceding LocalDecl
            if (pos > 0 && stmts[pos-1].kind == StmtKind::LocalDecl) {
                const CilStmt& decl = stmts[pos-1];
                if (decl.target && decl.target->isLocal()) {
                    varName  = decl.target->asLocal().name;
                    initExpr = decl.expr;
                }
            }
            return true;
        }
    }
    return false;
}

// ─── matchLockPattern ────────────────────────────────────────────────────────

bool CilPatternDetector::matchLockPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        CilExprPtr& lockExpr, std::vector<CilStmt>& body) {
    if (pos >= stmts.size()) return false;
    const CilStmt& s = stmts[pos];

    if (s.kind != StmtKind::Try) return false;
    if (s.finallyBody.empty()) return false;

    // Check for Monitor.Exit in finally
    for (const auto& fs : s.finallyBody) {
        if (fs.kind != StmtKind::ExprStmt || !fs.expr) continue;
        if (!fs.expr->isCall()) continue;
        const ExprCall& call = fs.expr->asCall();
        if ((call.className.find("Monitor") != std::string::npos ||
             call.className.find("Threading") != std::string::npos) &&
            call.methodName == "Exit") {
            body = s.tryBody;
            if (!call.args.empty()) lockExpr = call.args[0];
            return true;
        }
    }
    return false;
}

// ─── matchForEachPattern ─────────────────────────────────────────────────────

bool CilPatternDetector::matchForEachPattern(
        const std::vector<CilStmt>& stmts, size_t pos,
        std::string& varName, BcType& varType,
        CilExprPtr& collection, std::vector<CilStmt>& body) {
    if (pos >= stmts.size()) return false;
    const CilStmt& s = stmts[pos];

    // Detect: try { while (enumerator.MoveNext()) { var x = enumerator.Current; ... } }
    //         finally { enumerator.Dispose(); }
    if (s.kind != StmtKind::Try) return false;
    if (s.finallyBody.empty()) return false;

    // Check for Dispose in finally
    for (const auto& fs : s.finallyBody) {
        if (fs.kind != StmtKind::ExprStmt || !fs.expr) continue;
        if (!fs.expr->isCall()) continue;
        if (fs.expr->asCall().methodName == "Dispose") {
            // Found the foreach pattern; extract body
            body = s.tryBody;
            varName = "item"; // placeholder
            varType = types::ClrObject();
            return true;
        }
    }
    return false;
}

// ─── detectPropertyAccess ────────────────────────────────────────────────────

void CilPatternDetector::detectPropertyAccess(CilRecoveredMethod& method) const {
    if (!opts_.detectProperties) return;

    walkStmts(method.body, [&](CilStmt& stmt, std::vector<CilStmt>&) -> bool {
        auto pp = matchPropertyCall(stmt);
        if (!pp) return false;

        if (pp->kind == PropertyPattern::Getter) {
            // Rewrite: expr → obj.PropertyName
            if (stmt.expr && stmt.expr->isCall()) {
                const ExprCall& call = stmt.expr->asCall();
                ExprField ef;
                ef.obj       = call.obj;
                ef.className = call.className;
                ef.fieldName = pp->propertyName;
                ef.type      = call.retType;
                stmt.expr    = std::make_shared<CilExpr>(std::move(ef), call.retType);
                if (stmt.kind == StmtKind::ExprStmt)
                    method.isPropertyGetter = true;
            }
        } else if (pp->kind == PropertyPattern::Setter) {
            if (stmt.expr && stmt.expr->isCall()) {
                const ExprCall& call = stmt.expr->asCall();
                // Rewrite: callvirt set_Prop(val) → obj.Prop = val
                ExprField ef;
                ef.obj       = call.obj;
                ef.className = call.className;
                ef.fieldName = pp->propertyName;
                ef.type      = call.retType;
                stmt.kind   = StmtKind::Assign;
                stmt.target = std::make_shared<CilExpr>(std::move(ef), call.retType);
                stmt.expr   = pp->valueExpr;
                method.isPropertySetter = true;
            }
        }
        return false;
    });
}

// ─── detectAsyncStateMachine ─────────────────────────────────────────────────

std::vector<CilStmt> CilPatternDetector::reconstructAsyncBody(
        const BcClass& smClass,
        const BcModule& module) {
    (void)module;
    std::vector<CilStmt> body;

    // Find MoveNext method
    const BcMethod* moveNext = nullptr;
    for (const auto& m : smClass.methods) {
        if (m.name == "MoveNext") { moveNext = &m; break; }
    }
    if (!moveNext) return body;

    // Placeholder: in a full implementation, we would:
    // 1. Parse the switch on the state field
    // 2. Identify suspension points (awaiter.IsCompleted checks)
    // 3. Reconstruct each state as a segment of the original async method
    // 4. Insert `await` expressions at suspension points
    //
    // For now, emit a marker comment statement
    CilStmt placeholder;
    placeholder.kind = StmtKind::ExprStmt;
    placeholder.expr = nullptr; // TODO: full async reconstruction
    body.push_back(std::move(placeholder));

    return body;
}

std::vector<CilStmt> CilPatternDetector::reconstructIteratorBody(
        const BcClass& smClass,
        const BcModule& module) {
    (void)module;
    std::vector<CilStmt> body;

    const BcMethod* moveNext = nullptr;
    for (const auto& m : smClass.methods) {
        if (m.name == "MoveNext") { moveNext = &m; break; }
    }
    if (!moveNext) return body;

    // Placeholder for iterator reconstruction
    CilStmt placeholder;
    placeholder.kind = StmtKind::ExprStmt;
    placeholder.expr = nullptr;
    body.push_back(std::move(placeholder));

    return body;
}

void CilPatternDetector::detectAsyncStateMachine(
        CilRecoveredMethod& method,
        const BcModule& module) const {
    if (!opts_.detectAsync) return;

    // Look for: the method creates an async state machine (MoveNext + SetStateMachine)
    // Detected by presence of AsyncTaskMethodBuilder field accesses
    walkStmts(method.body, [&](CilStmt& stmt, std::vector<CilStmt>&) -> bool {
        if (!stmt.expr) return false;
        // Check if it's a call to AsyncTaskMethodBuilder.Start or similar
        if (stmt.expr->isCall()) {
            const ExprCall& call = stmt.expr->asCall();
            if (call.methodName == "Start" &&
                call.className.find("AsyncTaskMethodBuilder") != std::string::npos) {
                method.isAsync = true;
            }
        }
        return false;
    });

    // Search module for the state machine class
    if (method.method) {
        for (const auto& cls : module.classes()) {
            if (isAsyncSMClass(cls)) {
                // Check if related to this method (naming convention)
                std::string smName = "<" + std::string(method.method->name) + ">";
                if (cls.name.find(smName) != std::string::npos) {
                    method.isAsync = true;
                    // Note: Full reconstruction would replace method.body here
                }
            }
        }
    }
}

// ─── detectIteratorStateMachine ──────────────────────────────────────────────

void CilPatternDetector::detectIteratorStateMachine(
        CilRecoveredMethod& method,
        const BcModule& module) const {
    if (!opts_.detectIterator) return;

    if (method.method) {
        for (const auto& cls : module.classes()) {
            if (isIteratorSMClass(cls)) {
                std::string smName = "<" + std::string(method.method->name) + ">";
                if (cls.name.find(smName) != std::string::npos) {
                    method.isIterator = true;
                }
            }
        }
    }
}

// ─── detectLinqChains ────────────────────────────────────────────────────────

void CilPatternDetector::detectLinqChains(CilRecoveredMethod& method) const {
    if (!opts_.detectLinq) return;

    static const std::vector<std::string> linqMethods = {
        "Where", "Select", "SelectMany", "OrderBy", "OrderByDescending",
        "ThenBy", "ThenByDescending", "GroupBy", "GroupJoin", "Join",
        "Take", "Skip", "TakeWhile", "SkipWhile", "First", "FirstOrDefault",
        "Last", "LastOrDefault", "Single", "SingleOrDefault", "Count",
        "Any", "All", "Aggregate", "Sum", "Min", "Max", "Average",
        "Distinct", "Union", "Intersect", "Except", "Concat", "Zip",
        "ToList", "ToArray", "ToDictionary", "ToLookup", "AsEnumerable",
        "Cast", "OfType", "Reverse", "Contains", "Append", "Prepend",
        "DefaultIfEmpty", "ElementAt", "ElementAtOrDefault",
    };

    walkStmts(method.body, [&](CilStmt& stmt, std::vector<CilStmt>&) -> bool {
        if (!stmt.expr || !stmt.expr->isCall()) return false;
        const ExprCall& call = stmt.expr->asCall();
        bool isLinq = (call.className.find("Enumerable") != std::string::npos ||
                       call.className.find("Queryable") != std::string::npos);
        if (!isLinq) {
            isLinq = std::find(linqMethods.begin(), linqMethods.end(),
                               call.methodName) != linqMethods.end();
        }
        if (isLinq) method.hasLinq = true;
        return false;
    });
}

// ─── detectUnsafePatterns ────────────────────────────────────────────────────

void CilPatternDetector::detectUnsafePatterns(CilRecoveredMethod& method) const {
    if (!opts_.detectUnsafe) return;

    walkStmts(method.body, [&](CilStmt& stmt, std::vector<CilStmt>&) -> bool {
        if (!stmt.expr) return false;
        // localloc → stackalloc
        if (std::holds_alternative<ExprLocAlloc>(stmt.expr->data)) {
            method.hasUnsafe = true;
            if (stmt.kind == StmtKind::ExprStmt || stmt.kind == StmtKind::Assign) {
                stmt.kind = StmtKind::Stackalloc;
            }
        }
        // pinned locals detected during var recovery
        return false;
    });

    for (const auto& lv : method.locals) {
        if (lv.isPinned) {
            method.hasUnsafe = true;
            break;
        }
    }
}

// ─── detectIsPatterns ────────────────────────────────────────────────────────

void CilPatternDetector::detectIsPatterns(CilRecoveredMethod& method) const {
    if (!opts_.detectPatternMatch) return;

    for (size_t i = 0; i < method.body.size(); ++i) {
        auto pp = matchIsTypePattern(method.body, i);
        if (pp) {
            method.hasPatternMatch = true;
            // Could rewrite the next few statements here
        }
    }
}

// ─── detectSwitchExpressions ─────────────────────────────────────────────────

void CilPatternDetector::detectSwitchExpressions(CilRecoveredMethod& method) const {
    if (!opts_.detectSwitchExpr) return;

    // Detect compiler-emitted switch expressions (C# 8+):
    // The pattern is a switch statement where each case produces a value
    // stored to the same temporary local.
    for (auto& stmt : method.body) {
        if (stmt.kind != StmtKind::Switch) continue;
        if (stmt.cases.empty()) continue;

        bool allCasesAssign = std::all_of(stmt.cases.begin(), stmt.cases.end(),
            [](const CilStmt::SwitchCase& c) {
                return c.body.size() == 1 &&
                       (c.body[0].kind == StmtKind::Assign ||
                        c.body[0].kind == StmtKind::Return);
            });

        if (allCasesAssign) {
            // Could mark as SwitchExpr pattern for the emitter
        }
    }
}

// ─── detectUsingStatements ───────────────────────────────────────────────────

void CilPatternDetector::detectUsingStatements(CilRecoveredMethod& method) const {
    if (!opts_.detectUsing) return;

    std::vector<CilStmt> newBody;
    for (size_t i = 0; i < method.body.size(); ++i) {
        std::string varName;
        CilExprPtr initExpr;
        std::vector<CilStmt> body;
        if (matchUsingPattern(method.body, i, varName, initExpr, body)) {
            CilStmt us;
            us.kind        = StmtKind::Using;
            us.iterVarName = varName;
            us.expr        = initExpr;
            us.loopBody    = std::move(body);
            newBody.push_back(std::move(us));
            // Skip the preceding LocalDecl
            if (i > 0 && !newBody.empty() &&
                newBody.back().kind == StmtKind::LocalDecl) {
                newBody.pop_back();
            }
        } else {
            newBody.push_back(std::move(method.body[i]));
        }
    }
    method.body = std::move(newBody);
}

// ─── detectLockStatements ────────────────────────────────────────────────────

void CilPatternDetector::detectLockStatements(CilRecoveredMethod& method) const {
    if (!opts_.detectLock) return;

    std::vector<CilStmt> newBody;
    for (size_t i = 0; i < method.body.size(); ++i) {
        CilExprPtr lockExpr;
        std::vector<CilStmt> body;
        if (matchLockPattern(method.body, i, lockExpr, body)) {
            CilStmt ls;
            ls.kind     = StmtKind::Lock;
            ls.expr     = lockExpr;
            ls.loopBody = std::move(body);
            newBody.push_back(std::move(ls));
        } else {
            newBody.push_back(std::move(method.body[i]));
        }
    }
    method.body = std::move(newBody);
}

// ─── detectForEachLoops ──────────────────────────────────────────────────────

void CilPatternDetector::detectForEachLoops(CilRecoveredMethod& method) const {
    if (!opts_.detectForEach) return;

    std::vector<CilStmt> newBody;
    for (size_t i = 0; i < method.body.size(); ++i) {
        std::string varName;
        BcType varType;
        CilExprPtr collection;
        std::vector<CilStmt> body;
        if (matchForEachPattern(method.body, i, varName, varType, collection, body)) {
            CilStmt fe;
            fe.kind        = StmtKind::ForEach;
            fe.iterVarName = varName;
            fe.iterVarType = varType;
            fe.expr        = collection;
            fe.loopBody    = std::move(body);
            newBody.push_back(std::move(fe));
        } else {
            newBody.push_back(std::move(method.body[i]));
        }
    }
    method.body = std::move(newBody);
}

// ─── detect (master pass) ─────────────────────────────────────────────────────

void CilPatternDetector::detect(CilRecoveredMethod& method,
                                  const BcModule& module) const {
    // Run all passes in order
    if (opts_.detectProperties)   detectPropertyAccess(method);
    if (opts_.detectAsync)        detectAsyncStateMachine(method, module);
    if (opts_.detectIterator)     detectIteratorStateMachine(method, module);
    if (opts_.detectLinq)         detectLinqChains(method);
    if (opts_.detectUnsafe)       detectUnsafePatterns(method);
    if (opts_.detectPatternMatch) detectIsPatterns(method);
    if (opts_.detectSwitchExpr)   detectSwitchExpressions(method);
    if (opts_.detectUsing)        detectUsingStatements(method);
    if (opts_.detectLock)         detectLockStatements(method);
    if (opts_.detectForEach)      detectForEachLoops(method);
}

} // namespace cil_reconstruct
} // namespace retdec
