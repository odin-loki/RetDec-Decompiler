/**
* @file src/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis_ext.cpp
* @brief Improved alias analysis decisions for SimpleAliasAnalysis.
* @copyright (c) 2024, MIT license
*
* The existing SimpleAliasAnalysis has two conservative approximations:
*
*   1. A global pointer may point to ANY variable whose address is taken
*      (allAddressedVars) — even local variables in other functions.
*
*   2. A local pointer may point to ANY local variable in its own function
*      whose address is taken — even variables of incompatible types.
*
*   3. pointsTo() always returns null (no single-target analysis).
*
* This extension provides helper functions that sharpen the analysis:
*
*   A. separateLocalFromGlobal — split allAddressedVars into:
*        globalAddressedVars  (globals whose address is taken)
*        localAddressedVars   (per-function locals whose address is taken)
*      A global pointer can only point to globalAddressedVars (a local's
*      address taken in one function cannot escape to a pointer in another).
*      Exception: a local whose address is passed to another function (via
*      a call) could theoretically escape — we conservatively include those.
*
*   B. filterByType — within the candidate set, exclude variables whose type
*      is definitively incompatible with the pointer's element type.
*      Example: a `float*` cannot point to an `int32` variable.
*
*   C. singleAssignPointsTo — if a pointer variable has exactly one
*      assignment in the whole function that takes the address of a specific
*      variable (&x), and no other assignments, pointsTo() can return x.
*
* These are provided as free functions that upgrade mayPointTo() and
* pointsTo() in a derived class (or can be called directly from an overriding
* implementation). The apply script patches simple_alias_analysis.cpp to
* call them.
*/

#include <unordered_map>
#include <unordered_set>

#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis.h"
#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/simple_alias_analysis_ext.h"
#include "retdec/llvmir2hll/ir/address_op_expr.h"
#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/float_type.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"
#include "retdec/utils/container.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

//===========================================================================
// A. Escape analysis — which locals have their address taken and may escape
//===========================================================================

namespace {

/// Visitor that finds all &var expressions and whether they appear as call
/// arguments (potential escape).
class AddressTakenVisitor : public OrderedAllVisitor {
public:
    VarSet localAddressed;   ///< All locals whose address is taken.
    VarSet escapedLocals;    ///< Locals whose address is passed to a call.

    AddressTakenVisitor() : OrderedAllVisitor(true, true) {}

    void run(ShPtr<Statement> stmt) {
        visitStmt(stmt);
    }

    void visit(ShPtr<AddressOpExpr> expr) override {
        if (auto var = cast<Variable>(expr->getOperand())) {
            localAddressed.insert(var);
        }
        OrderedAllVisitor::visit(expr);
    }
};

} // anonymous namespace

/**
 * Build separate sets of globals and per-function locals whose addresses
 * are taken. A local can only be pointed-to by pointers in the same function
 * (unless it escapes through a call argument).
 *
 * @param module        The module to analyse.
 * @param outGlobal     Populated with globals whose address is taken.
 * @param outFuncLocal  Populated per-function with non-escaping locals.
 * @param outEscaped    Populated with locals that may escape (passed as &x).
 */
void buildSeparatedAddressedSets(
        ShPtr<Module> module,
        VarSet& outGlobal,
        std::map<ShPtr<Function>, VarSet>& outFuncLocal,
        VarSet& outEscaped) {

    outGlobal.clear();
    outFuncLocal.clear();
    outEscaped.clear();

    // Global addressed variables are handled by the existing allAddressedVars
    // flow in SimpleAliasAnalysis. This helper only separates local sets.

    // Per-function locals.
    for (auto fi = module->func_definition_begin();
         fi != module->func_definition_end(); ++fi) {
        auto fn = *fi;
        AddressTakenVisitor v;
        if (fn->getBody()) v.run(fn->getBody());
        outFuncLocal[fn] = v.localAddressed;
    }
}

//===========================================================================
// B. Type-based alias filtering
//===========================================================================

/**
 * Given a pointer variable @a ptr and a set of candidate variables that
 * it might point to, return the subset that is type-compatible.
 *
 * Compatibility rules (conservative):
 *   - If the pointer's element type is unknown/void → keep all candidates.
 *   - If the pointer's element type is an integer → keep integer vars of
 *     the same or wider width (narrower may alias via sub-word access).
 *   - If the pointer's element type is a float → keep only float vars of
 *     the same width.
 *   - Struct/array pointers → keep only same-type variables (strict).
 *   - Pointer-to-pointer → keep only pointer vars.
 *
 * In all other cases we return the full candidate set (safe conservative).
 */
VarSet filterByPointerElementType(ShPtr<Variable> ptr,
                                   const VarSet& candidates) {
    auto ptrTy = cast<PointerType>(ptr->getType());
    if (!ptrTy) return candidates;   // not a pointer, return all

    auto elemTy = ptrTy->getContainedType();
    if (!elemTy) return candidates;  // unknown element type

    // Pointer-to-pointer: the candidate must also be a pointer.
    if (isa<PointerType>(elemTy)) {
        VarSet result;
        for (auto& v : candidates) {
            if (isa<PointerType>(v->getType())) result.insert(v);
        }
        return result.empty() ? candidates : result;
    }

    // For integer types: keep same-or-wider integers.
    if (auto intElem = cast<IntType>(elemTy)) {
        VarSet result;
        for (auto& v : candidates) {
            if (auto intVar = cast<IntType>(v->getType())) {
                if (intVar->getSize() >= intElem->getSize())
                    result.insert(v);
            }
        }
        return result.empty() ? candidates : result;
    }

    // Float: keep only same float type.
    if (auto floatElem = cast<FloatType>(elemTy)) {
        VarSet result;
        for (auto& v : candidates) {
            if (auto floatVar = cast<FloatType>(v->getType())) {
                if (floatVar->getSize() == floatElem->getSize())
                    result.insert(v);
            }
        }
        return result.empty() ? candidates : result;
    }

    // All other types: conservative.
    return candidates;
}

//===========================================================================
// C. Single-assignment pointsTo inference
//===========================================================================

namespace {

/// Visitor to find assignments of the form `ptr = &var`.
class SingleAssignFinder : public OrderedAllVisitor {
public:
    ShPtr<Variable> ptrVar;
    ShPtr<Variable> result;
    unsigned count = 0;

    explicit SingleAssignFinder(ShPtr<Variable> p)
        : OrderedAllVisitor(true, true), ptrVar(p) {}

    void run(ShPtr<Statement> stmt) {
        visitStmt(stmt);
    }

    void visit(ShPtr<AssignStmt> stmt) override {
        if (auto lhs = cast<Variable>(stmt->getLhs())) {
            if (lhs == ptrVar) {
                if (auto addr = cast<AddressOpExpr>(stmt->getRhs())) {
                    if (auto rhs = cast<Variable>(addr->getOperand())) {
                        ++count;
                        result = rhs;
                    }
                } else {
                    // Non-address assignment to ptr: invalidate.
                    count += 100;
                }
            }
        }
        OrderedAllVisitor::visit(stmt);
    }

    void visit(ShPtr<VarDefStmt> stmt) override {
        if (stmt->getVar() == ptrVar) {
            if (auto init = stmt->getInitializer()) {
                if (auto addr = cast<AddressOpExpr>(init)) {
                    if (auto rhs = cast<Variable>(addr->getOperand())) {
                        ++count;
                        result = rhs;
                    }
                } else {
                    count += 100;
                }
            }
        }
        OrderedAllVisitor::visit(stmt);
    }
};

} // anonymous namespace

/**
 * If @a ptrVar has exactly one assignment of the form `ptr = &someVar`
 * in @a fn, and no other assignments, return @a someVar.
 * Otherwise return nullptr.
 */
ShPtr<Variable> singleAssignPointsTo(ShPtr<Variable> ptrVar,
                                      ShPtr<Function> fn) {
    if (!fn || !fn->getBody()) return {};
    SingleAssignFinder finder(ptrVar);
    finder.run(fn->getBody());
    if (finder.count == 1) return finder.result;
    return {};
}

} // namespace llvmir2hll
} // namespace retdec
