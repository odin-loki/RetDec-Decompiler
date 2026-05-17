/**
* @file src/llvmir2hll/optimizer/optimizers/unknown_type_inferrer.cpp
* @brief Infer concrete types for variables whose type is UnknownType.
* @copyright (c) 2024, MIT license
*/

#include <unordered_map>
#include <unordered_set>
#include <functional>

#include "retdec/llvmir2hll/optimizer/optimizers/unknown_type_inferrer.h"

#include "retdec/llvmir2hll/ir/assign_stmt.h"
#include "retdec/llvmir2hll/ir/call_expr.h"
#include "retdec/llvmir2hll/ir/call_stmt.h"
#include "retdec/llvmir2hll/ir/const_null_pointer.h"
#include "retdec/llvmir2hll/ir/deref_op_expr.h"
#include "retdec/llvmir2hll/ir/array_index_op_expr.h"
#include "retdec/llvmir2hll/ir/address_op_expr.h"
#include "retdec/llvmir2hll/ir/eq_op_expr.h"
#include "retdec/llvmir2hll/ir/neq_op_expr.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/if_stmt.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/return_stmt.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/unknown_type.h"
#include "retdec/llvmir2hll/ir/var_def_stmt.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/ir/void_type.h"
#include "retdec/llvmir2hll/ir/while_loop_stmt.h"
#include "retdec/llvmir2hll/ir/for_loop_stmt.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

namespace {

/// Returns true if @a type is still unresolved.
bool isUnknown(ShPtr<Type> type) {
    return isa<UnknownType>(type);
}

/// Returns true if @a type is a concrete (useful) type.
bool isConcrete(ShPtr<Type> type) {
    return type && !isa<UnknownType>(type);
}

/// Returns the "better" of two candidate types: concrete wins over unknown.
ShPtr<Type> betterType(ShPtr<Type> a, ShPtr<Type> b) {
    if (isConcrete(a)) return a;
    if (isConcrete(b)) return b;
    return nullptr;
}

//===========================================================================
// Constraint collector — walks a single function body and collects
// (variable → candidate type) pairs from usage contexts.
//===========================================================================

class ConstraintCollector {
public:
    explicit ConstraintCollector(ShPtr<Module> module) : module(module) {}

    /// Collect constraints from all statements in @a func.
    /// Returns a map from variable to the best inferred type.
    std::unordered_map<ShPtr<Variable>, ShPtr<Type>>
    collect(ShPtr<Function> func) {
        candidates.clear();
        walkStmts(func->getBody());
        return candidates;
    }

private:
    ShPtr<Module> module;
    std::unordered_map<ShPtr<Variable>, ShPtr<Type>> candidates;

    //-----------------------------------------------------------------------
    // Helpers
    //-----------------------------------------------------------------------

    void propose(ShPtr<Expression> expr, ShPtr<Type> type) {
        if (!expr || !type || !isConcrete(type)) return;
        if (auto var = cast<Variable>(expr)) {
            if (isUnknown(var->getType())) {
                auto &slot = candidates[var];
                slot = betterType(slot, type) ? betterType(slot, type) : type;
            }
        }
    }

    /// If @a expr is a Variable with UnknownType, propose void* for it.
    void proposeVoidPtr(ShPtr<Expression> expr) {
        propose(expr, PointerType::create(VoidType::create()));
    }

    /// If @a expr is a Variable with UnknownType, propose int32_t for it.
    void proposeInt32(ShPtr<Expression> expr) {
        propose(expr, IntType::create(32, true));
    }

    //-----------------------------------------------------------------------
    // Expression walking
    //-----------------------------------------------------------------------

    void walkExpr(ShPtr<Expression> expr) {
        if (!expr) return;

        // DerefOpExpr: *v → v must be a pointer to something
        if (auto deref = cast<DerefOpExpr>(expr)) {
            auto operand = deref->getOperand();
            // The contained type can't be inferred from this alone; use void*
            proposeVoidPtr(operand);
            walkExpr(operand);
            return;
        }

        // ArrayIndexOpExpr: base[index] → base is a pointer, index is int
        if (auto ai = cast<ArrayIndexOpExpr>(expr)) {
            proposeVoidPtr(ai->getBase());
            proposeInt32(ai->getIndex());
            walkExpr(ai->getBase());
            walkExpr(ai->getIndex());
            return;
        }

        // AddressOpExpr: &v → v's type is whatever v is; skip
        if (auto addr = cast<AddressOpExpr>(expr)) {
            walkExpr(addr->getOperand());
            return;
        }

        // EqOpExpr / NeqOpExpr against NULL → pointer
        if (auto eq = cast<EqOpExpr>(expr)) {
            if (isa<ConstNullPointer>(eq->getFirstOperand()))
                proposeVoidPtr(eq->getSecondOperand());
            else if (isa<ConstNullPointer>(eq->getSecondOperand()))
                proposeVoidPtr(eq->getFirstOperand());
            walkExpr(eq->getFirstOperand());
            walkExpr(eq->getSecondOperand());
            return;
        }
        if (auto neq = cast<NeqOpExpr>(expr)) {
            if (isa<ConstNullPointer>(neq->getFirstOperand()))
                proposeVoidPtr(neq->getSecondOperand());
            else if (isa<ConstNullPointer>(neq->getSecondOperand()))
                proposeVoidPtr(neq->getFirstOperand());
            walkExpr(neq->getFirstOperand());
            walkExpr(neq->getSecondOperand());
            return;
        }

        // CallExpr: match arguments against known function parameter types
        if (auto call = cast<CallExpr>(expr)) {
            if (auto calledVar = cast<Variable>(call->getCalledExpr())) {
                auto func = module->getFuncByName(calledVar->getName());
                if (func && func->isDefinition()) {
                    const auto &params = func->getParams();
                    const auto &args = call->getArgs();
                    std::size_t n = std::min(params.size(), args.size());
                    for (std::size_t i = 0; i < n; ++i) {
                        ShPtr<Type> paramType = params[i]->getType();
                        if (isConcrete(paramType)) {
                            propose(args[i], paramType);
                        }
                    }
                }
            }
            for (auto &arg : call->getArgs()) walkExpr(arg);
            return;
        }

        // BinaryOpExpr — walk both sides; cross-propagate if one side is typed
        if (auto bin = cast<BinaryOpExpr>(expr)) {
            auto lhsType = bin->getFirstOperand()->getType();
            auto rhsType = bin->getSecondOperand()->getType();
            if (isConcrete(lhsType)) propose(bin->getSecondOperand(), lhsType);
            if (isConcrete(rhsType)) propose(bin->getFirstOperand(), rhsType);
            walkExpr(bin->getFirstOperand());
            walkExpr(bin->getSecondOperand());
            return;
        }

        // UnaryOpExpr — walk operand
        if (auto un = cast<UnaryOpExpr>(expr)) {
            walkExpr(un->getOperand());
            return;
        }
    }

    //-----------------------------------------------------------------------
    // Statement walking
    //-----------------------------------------------------------------------

    void walkStmts(ShPtr<Statement> stmt) {
        while (stmt) {
            walkOneStmt(stmt);
            stmt = stmt->getSuccessor();
        }
    }

    void walkOneStmt(ShPtr<Statement> stmt) {
        if (!stmt) return;

        // AssignStmt: lhs = rhs → propagate type across
        if (auto assign = cast<AssignStmt>(stmt)) {
            walkExpr(assign->getLhs());
            walkExpr(assign->getRhs());
            auto lhsType = assign->getLhs()->getType();
            auto rhsType = assign->getRhs()->getType();
            if (isConcrete(rhsType)) propose(assign->getLhs(), rhsType);
            if (isConcrete(lhsType)) propose(assign->getRhs(), lhsType);
            return;
        }

        // VarDefStmt: T v = init → if init has known type, v gets it
        if (auto vd = cast<VarDefStmt>(stmt)) {
            if (auto init = vd->getInitializer()) {
                walkExpr(init);
                auto initType = init->getType();
                if (isConcrete(initType)) propose(vd->getVar(), initType);
                // Reverse: if var has concrete type, propose to init
                auto varType = vd->getVar()->getType();
                if (isConcrete(varType)) propose(init, varType);
            }
            return;
        }

        // CallStmt: walk the embedded call expression
        if (auto cs = cast<CallStmt>(stmt)) {
            walkExpr(cs->getCall());
            return;
        }

        // ReturnStmt: walk the return value
        if (auto rs = cast<ReturnStmt>(stmt)) {
            if (rs->getRetVal()) walkExpr(rs->getRetVal());
            return;
        }

        // IfStmt: walk condition and all branches
        if (auto is = cast<IfStmt>(stmt)) {
            for (auto ci = is->clause_begin(); ci != is->clause_end(); ++ci) {
                walkExpr(ci->first);
                walkStmts(ci->second);
            }
            if (is->hasElseClause()) walkStmts(is->getElseClause());
            return;
        }

        // WhileLoopStmt
        if (auto wl = cast<WhileLoopStmt>(stmt)) {
            walkExpr(wl->getCondition());
            walkStmts(wl->getBody());
            return;
        }

        // ForLoopStmt
        if (auto fl = cast<ForLoopStmt>(stmt)) {
            walkStmts(fl->getBody());
            return;
        }
    }
};

} // anonymous namespace

//===========================================================================
// UnknownTypeInferrer
//===========================================================================

UnknownTypeInferrer::UnknownTypeInferrer(ShPtr<Module> module)
    : FuncOptimizer(module) {
    PRECONDITION_NON_NULL(module);
}

void UnknownTypeInferrer::doOptimization() {
    ConstraintCollector collector(module);

    // Fixed-point iteration: repeat until no variable's type changes.
    bool changed = true;
    while (changed) {
        changed = false;

        for (auto fi = module->func_begin(), fe = module->func_end();
                fi != fe; ++fi) {
            ShPtr<Function> func = *fi;
            if (!func->isDefinition()) continue;

            auto candidates = collector.collect(func);

            for (auto &[var, type] : candidates) {
                if (isUnknown(var->getType()) && isConcrete(type)) {
                    var->setType(type);
                    changed = true;
                }
            }
        }
    }

    // Final pass: any remaining UnknownType → void *
    auto voidPtr = PointerType::create(VoidType::create());
    for (auto fi = module->func_begin(), fe = module->func_end();
            fi != fe; ++fi) {
        ShPtr<Function> func = *fi;
        if (!func->isDefinition()) continue;

        // Local variables (including params)
        VarSet locals = func->getLocalVars(/*includeParams=*/true);
        for (auto &var : locals) {
            if (isUnknown(var->getType())) {
                var->setType(voidPtr);
            }
        }
    }

    // Global variables
    for (auto &var : module->getGlobalVars()) {
        if (isUnknown(var->getType())) {
            var->setType(voidPtr);
        }
    }
}

} // namespace llvmir2hll
} // namespace retdec
