/**
 * @file src/cxx_backend/cxx_lifter.cpp
 * @brief C++ pattern detection and CUnit → CxxUnit lifting.
 */

#include "retdec/cxx_backend/cxx_lifter.h"

#include <algorithm>
#include <regex>

namespace retdec {
namespace cxx_backend {

// ─── VtableDetector ──────────────────────────────────────────────────────────

std::string VtableDetector::className(const VtableEntry& vt) const {
    if (!vt.className.empty()) return vt.className;
    // Derive from RTTI symbol: strip _ZTS or _ZTI prefix
    std::string sym = vt.rttiSymbol;
    if (sym.substr(0, 4) == "_ZTS" || sym.substr(0, 4) == "_ZTI")
        sym = sym.substr(4);
    // Simple Itanium demangling: strip length prefix
    while (!sym.empty() && std::isdigit((unsigned char)sym[0])) {
        size_t len = 0;
        size_t i = 0;
        while (i < sym.size() && std::isdigit((unsigned char)sym[i]))
            len = len * 10 + (sym[i++] - '0');
        if (i + len <= sym.size()) {
            return sym.substr(i, len);
        }
        break;
    }
    return sym.empty() ? "UnknownClass" : sym;
}

bool VtableDetector::isVtableStore(const codegen::CFunction& fn,
                                     const VtableEntry& vt) const {
    // Heuristic: check if any expression in the function body matches
    // *(param0 + 0) = &vtable_symbol
    // This is a simplified check on function name patterns.
    const std::string& name = fn.name;
    // Constructors typically contain the class name
    std::string cn = className(vt);
    return !cn.empty() && name.find(cn) != std::string::npos;
}

std::vector<CxxClass> VtableDetector::detect(const codegen::CUnit& unit,
                                               const CxxLiftContext& ctx) const {
    std::vector<CxxClass> classes;

    for (const auto& vt : ctx.vtables) {
        CxxClass cls;
        cls.name = className(vt);
        cls.kind = CxxClass::Kind::Class;

        // Add virtual methods
        for (size_t vi = 0; vi < vt.virtualFunctions.size(); ++vi) {
            CxxMethod m;
            m.name      = vt.virtualFunctions[vi];
            m.isVirtual = true;
            m.returnType = CType::make(CType::Kind::Void);
            // Look up actual return type from CUnit functions
            for (const auto& fn : unit.functions) {
                if (fn.name == m.name) {
                    if (fn.returnType) m.returnType = fn.returnType;
                    m.params = fn.params;
                    m.body   = fn.body;
                    break;
                }
            }
            cls.methods.push_back(std::move(m));
        }

        classes.push_back(std::move(cls));
    }

    return classes;
}

// ─── CtorDtorDetector ────────────────────────────────────────────────────────

// Itanium mangling: constructors end with C1/C2, destructors with D1/D2/D0
static bool mangledIsCtor(const std::string& name) {
    static const std::regex ctorRe(R"(_ZN.*C[12]E)");
    return std::regex_search(name, ctorRe);
}

static bool mangledIsDtor(const std::string& name) {
    static const std::regex dtorRe(R"(_ZN.*D[012]E)");
    return std::regex_search(name, dtorRe);
}

bool CtorDtorDetector::looksLikeConstructor(const codegen::CFunction& fn,
                                              const CxxLiftContext& ctx) const {
    // Check mangled name first
    if (mangledIsCtor(fn.name)) return true;
    // Check demangled name
    auto it = ctx.demangledNames.find(fn.name);
    if (it != ctx.demangledNames.end()) {
        const std::string& dem = it->second;
        // Demangled constructors look like "ClassName::ClassName(...)"
        auto colonPos = dem.find("::");
        if (colonPos != std::string::npos) {
            std::string prefix = dem.substr(0, colonPos);
            std::string rest   = dem.substr(colonPos + 2);
            if (rest.find(prefix) == 0) return true;
        }
    }
    return false;
}

bool CtorDtorDetector::looksLikeDestructor(const codegen::CFunction& fn,
                                             const CxxLiftContext& ctx) const {
    if (mangledIsDtor(fn.name)) return true;
    auto it = ctx.demangledNames.find(fn.name);
    if (it != ctx.demangledNames.end()) {
        return it->second.find("~") != std::string::npos;
    }
    return false;
}

CtorDtorDetector::Result CtorDtorDetector::detect(
        const codegen::CUnit& unit, const CxxLiftContext& ctx) const {
    Result res;
    for (const auto& fn : unit.functions) {
        if (looksLikeConstructor(fn, ctx))
            res.constructors.insert(fn.name);
        else if (looksLikeDestructor(fn, ctx))
            res.destructors.insert(fn.name);
    }
    return res;
}

// ─── NewDeleteRecovery ────────────────────────────────────────────────────────

bool NewDeleteRecovery::isMallocCall(const codegen::CExpr& e,
                                      size_t* /*sizeOut*/) const {
    return e.kind == codegen::CExpr::Kind::Call && e.callee == "malloc";
}

bool NewDeleteRecovery::isFreeCall(const codegen::CExpr& e) const {
    return e.kind == codegen::CExpr::Kind::Call && e.callee == "free";
}

std::vector<NewDeleteRecovery::Replacement>
NewDeleteRecovery::analyse(const codegen::CUnit& unit) const {
    std::vector<Replacement> reps;
    for (const auto& fn : unit.functions) {
        if (!fn.body) continue;
        const auto& stmts = fn.body->children;
        for (size_t i = 0; i < stmts.size(); ++i) {
            const auto& s = stmts[i];
            // Look for: decl/assign = (T*)malloc(sizeof(T))
            if (s->kind == codegen::CStmt::Kind::Assign && s->expr) {
                if (isMallocCall(*s->expr)) {
                    Replacement r;
                    r.functionName = fn.name;
                    r.stmtIndex    = i;
                    r.isNew        = true;
                    r.isArray      = false;
                    reps.push_back(r);
                }
            }
            // Look for: free(ptr)
            if (s->kind == codegen::CStmt::Kind::ExprStmt && s->expr) {
                if (isFreeCall(*s->expr)) {
                    Replacement r;
                    r.functionName = fn.name;
                    r.stmtIndex    = i;
                    r.isNew        = false;
                    r.isArray      = false;
                    reps.push_back(r);
                }
            }
        }
    }
    return reps;
}

codegen::CFunction NewDeleteRecovery::applyToFunction(
        codegen::CFunction fn,
        const std::vector<Replacement>& reps) const {
    // For now: annotate with comments (full AST rewriting would be a separate pass)
    if (!fn.body) return fn;
    for (const auto& r : reps) {
        if (r.functionName != fn.name) continue;
        // Insert a comment statement before the replaced statement
        auto comment = codegen::CStmt::exprStmt(
            codegen::CExpr::lit(r.isNew ? "/* new */" : "/* delete */"));
        if (r.stmtIndex < fn.body->children.size()) {
            fn.body->children.insert(
                fn.body->children.begin() + r.stmtIndex, comment);
        }
    }
    return fn;
}

// ─── EhRecovery ──────────────────────────────────────────────────────────────

void EhRecovery::applyToFunction(codegen::CFunction& fn,
                                   const std::vector<EhRegion>& regions,
                                   std::vector<CxxTryStmt>& triesOut) const {
    if (!fn.body || regions.empty()) return;

    for (const auto& region : regions) {
        CxxTryStmt tryStmt;
        // Create try body (placeholder block)
        tryStmt.tryBody = codegen::CStmt::block();

        // Create catch clause
        CxxCatchClause clause;
        if (region.catchType.empty() || region.catchType == "...") {
            clause.exceptionType = nullptr; // catch(...)
        } else {
            auto t = CType::make(CType::Kind::Struct);
            t->name = region.catchType;
            clause.exceptionType = t;
        }
        clause.varName = region.catchVar;
        clause.body    = codegen::CStmt::block();
        tryStmt.catches.push_back(std::move(clause));
        triesOut.push_back(std::move(tryStmt));
    }
}

// ─── NamespaceGrouper ────────────────────────────────────────────────────────

std::string NamespaceGrouper::extractNamespace(const std::string& dem) const {
    // Look for the last "::" before the function/class name
    auto pos = dem.rfind("::");
    if (pos == std::string::npos) return "";
    return dem.substr(0, pos);
}

std::vector<CxxNamespace> NamespaceGrouper::group(
        const std::vector<CxxClass>& classes,
        const std::vector<codegen::CFunction>& fns,
        const CxxLiftContext& ctx) const {
    // Build map: namespace → {classes, functions}
    std::unordered_map<std::string, CxxNamespace> nsMap;

    auto getOrCreate = [&](const std::string& name) -> CxxNamespace& {
        auto it = nsMap.find(name);
        if (it == nsMap.end()) {
            nsMap[name] = CxxNamespace{name, false, ""};
            return nsMap[name];
        }
        return it->second;
    };

    for (const auto& cls : classes) {
        auto it = ctx.demangledNames.find(cls.name);
        if (it != ctx.demangledNames.end()) {
            std::string ns = extractNamespace(it->second);
            if (!ns.empty()) {
                getOrCreate(ns).contents += "class " + cls.name + ";\n";
            }
        }
    }

    for (const auto& fn : fns) {
        auto it = ctx.demangledNames.find(fn.name);
        if (it != ctx.demangledNames.end()) {
            std::string ns = extractNamespace(it->second);
            if (!ns.empty()) {
                getOrCreate(ns).contents += fn.name + ";\n";
            }
        }
    }

    std::vector<CxxNamespace> result;
    for (auto& [name, ns] : nsMap)
        result.push_back(std::move(ns));
    return result;
}

// ─── Includes helper ─────────────────────────────────────────────────────────

void CxxLifter::populateIncludes(CxxUnit& unit, bool hasCxx) const {
    // Always include common C headers
    unit.systemIncludes = {"cstdint", "cstddef", "cstring"};
    if (hasCxx) {
        unit.systemIncludes.push_back("memory");
        unit.systemIncludes.push_back("stdexcept");
        unit.systemIncludes.push_back("string");
    }
}

// ─── CxxLifter ───────────────────────────────────────────────────────────────

CxxLifter::CxxLifter(CxxLiftOptions opts)
    : opts_(std::move(opts)) {}

CxxLiftResult CxxLifter::lift(const codegen::CUnit& cUnit,
                               const CxxLiftContext& ctx) const {
    CxxLiftResult result;
    CxxUnit& unit = result.unit;

    unit.filename = cUnit.filename.empty() ? "output" : cUnit.filename;
    unit.globalDecls = cUnit.globalDecls;

    // Start with all C functions as-is
    unit.functions = cUnit.functions;

    // Detect C++ evidence
    bool hasCxx = ctx.hasCxxEvidence || !ctx.vtables.empty() ||
                  !ctx.ehRegions.empty();

    // 1. Vtable detection
    if (opts_.detectVtables && !ctx.vtables.empty()) {
        VtableDetector vd;
        unit.classes = vd.detect(cUnit, ctx);
        hasCxx = true;
    }

    // 2. Constructor/Destructor detection
    CtorDtorDetector::Result cdResult;
    if (opts_.detectCtorDtor) {
        CtorDtorDetector cdd;
        cdResult = cdd.detect(cUnit, ctx);
        if (!cdResult.constructors.empty() || !cdResult.destructors.empty())
            hasCxx = true;
        // Tag methods in classes
        for (auto& cls : unit.classes) {
            for (auto& m : cls.methods) {
                if (cdResult.constructors.count(m.name)) {
                    m.isConstructor = true;
                }
                if (cdResult.destructors.count(m.name)) {
                    m.isDestructor = true;
                    m.isVirtual    = true; // destructors in polymorphic classes are virtual
                }
            }
        }
    }

    // 3. new/delete recovery
    if (opts_.recoverNewDelete) {
        NewDeleteRecovery ndr;
        auto reps = ndr.analyse(cUnit);
        if (!reps.empty()) hasCxx = true;
        // Apply replacements to functions
        for (auto& fn : unit.functions) {
            std::vector<NewDeleteRecovery::Replacement> fnReps;
            for (const auto& r : reps)
                if (r.functionName == fn.name) fnReps.push_back(r);
            if (!fnReps.empty())
                fn = ndr.applyToFunction(std::move(fn), fnReps);
        }
    }

    // 4. Exception handling recovery
    if (opts_.recoverExceptions && !ctx.ehRegions.empty()) {
        EhRecovery ehr;
        for (auto& fn : unit.functions) {
            auto it = ctx.ehRegions.find(fn.name);
            if (it != ctx.ehRegions.end()) {
                std::vector<CxxTryStmt> tries;
                ehr.applyToFunction(fn, it->second, tries);
                hasCxx = !tries.empty() || hasCxx;
            }
        }
    }

    // 5. Namespace grouping
    if (opts_.groupNamespaces && hasCxx) {
        NamespaceGrouper ng;
        unit.namespaces = ng.group(unit.classes, unit.functions, ctx);
    }

    // Includes
    populateIncludes(unit, hasCxx);

    unit.isCxx = hasCxx;
    result.isCxx = hasCxx;
    return result;
}

} // namespace cxx_backend
} // namespace retdec
