/**
* @file include/retdec/llvmir2hll/analysis/alias_analysis/alias_analyses/basic_alias_analysis.h
* @brief A basic alias analysis.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_ANALYSIS_ALIAS_ANALYSIS_ALIAS_ANALYSES_BASIC_ALIAS_ANALYSIS_H
#define RETDEC_LLVMIR2HLL_ANALYSIS_ALIAS_ANALYSIS_ALIAS_ANALYSES_BASIC_ALIAS_ANALYSIS_H

#include <string>

#include "retdec/llvmir2hll/analysis/alias_analysis/alias_analysis.h"
#include "retdec/llvmir2hll/ir/address_op_expr.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"
#include "retdec/llvmir2hll/support/types.h"
#include "retdec/llvmir2hll/support/visitors/ordered_all_visitor.h"

namespace retdec {
namespace llvmir2hll {

class Module;

/**
* @brief A conservative "top" alias analysis.
*
* Every pointer variable is assumed to potentially alias every variable
* whose address has been taken anywhere in the module.  This is less
* precise than SimpleAliasAnalysis (no per-function tracking) but is
* always safe.
*
* Use create() to create instances. Instances of this class have
* reference object semantics.
*/
class BasicAliasAnalysis: public AliasAnalysis, private OrderedAllVisitor {
public:
	static ShPtr<AliasAnalysis> create();

	virtual void init(ShPtr<Module> module) override;
	virtual std::string getId() const override;

	virtual const VarSet &mayPointTo(ShPtr<Variable> var) const override;
	virtual ShPtr<Variable> pointsTo(ShPtr<Variable> var) const override;
	virtual bool mayBePointed(ShPtr<Variable> var) const override;

	virtual void visit(ShPtr<AddressOpExpr> expr) override;

private:
	BasicAliasAnalysis();

	/// All variables whose address is taken anywhere in the module.
	VarSet addressTakenVars;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
