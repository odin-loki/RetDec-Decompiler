/**
* @file src/llvmir2hll/analysis/alias_analysis/alias_analyses/basic_alias_analysis.cpp
* @brief Implementation of BasicAliasAnalysis.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*
* This is a conservative "top" alias analysis: every pointer variable is
* assumed to potentially alias every variable whose address has been taken
* anywhere in the module.  It is less precise than SimpleAliasAnalysis (no
* per-function tracking) but is always safe and never returns a null pointer
* from create().
*/

#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/basic_alias_analysis.h"
#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analysis_factory.h"
#include "retdec/llvmir2hll/ir/address_op_expr.h"
#include "retdec/llvmir2hll/ir/function.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/ir/pointer_type.h"
#include "retdec/llvmir2hll/ir/statement.h"
#include "retdec/llvmir2hll/ir/variable.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/utils/container.h"

using retdec::utils::hasItem;

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("basic", BASIC_ALIAS_ANALYSIS_ID, AliasAnalysisFactory,
	BasicAliasAnalysis::create);

namespace {
/// Shared empty set returned for non-pointer variables.
const VarSet EMPTY_VAR_SET;
}

BasicAliasAnalysis::BasicAliasAnalysis()
    : AliasAnalysis(), OrderedAllVisitor(true, true) {}

ShPtr<AliasAnalysis> BasicAliasAnalysis::create() {
	return ShPtr<BasicAliasAnalysis>(new BasicAliasAnalysis());
}

std::string BasicAliasAnalysis::getId() const {
	return BASIC_ALIAS_ANALYSIS_ID;
}

void BasicAliasAnalysis::init(ShPtr<Module> module) {
	AliasAnalysis::init(module);
	addressTakenVars.clear();
	restart();

	// Collect variables whose address is taken in global initializers.
	for (auto i = module->global_var_begin(), e = module->global_var_end();
	     i != e; ++i) {
		if (ShPtr<Expression> init = (*i)->getInitializer())
			init->accept(this);
	}

	// Collect variables whose address is taken anywhere in function bodies.
	for (auto i = module->func_definition_begin(),
	     e = module->func_definition_end(); i != e; ++i) {
		visitStmt((*i)->getBody());
	}
}

const VarSet &BasicAliasAnalysis::mayPointTo(ShPtr<Variable> var) const {
	if (!isa<PointerType>(var->getType()))
		return EMPTY_VAR_SET;
	// Conservative: any pointer may alias any variable whose address is taken.
	return addressTakenVars;
}

ShPtr<Variable> BasicAliasAnalysis::pointsTo(ShPtr<Variable> var) const {
	// Conservative: we cannot determine a unique points-to target.
	return ShPtr<Variable>();
}

bool BasicAliasAnalysis::mayBePointed(ShPtr<Variable> var) const {
	return hasItem(addressTakenVars, var);
}

void BasicAliasAnalysis::visit(ShPtr<AddressOpExpr> expr) {
	if (ShPtr<Variable> v = cast<Variable>(expr->getOperand()))
		addressTakenVars.insert(v);
	else
		OrderedAllVisitor::visit(expr);
}

} // namespace llvmir2hll
} // namespace retdec
