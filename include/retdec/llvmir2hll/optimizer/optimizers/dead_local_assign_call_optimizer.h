/**
 * @file include/retdec/llvmir2hll/optimizer/optimizers/dead_local_assign_call_optimizer.h
 * @copyright (c) 2024, MIT license
 */
#ifndef RETDEC_DEAD_LOCAL_ASSIGN_CALL_OPTIMIZER_H
#define RETDEC_DEAD_LOCAL_ASSIGN_CALL_OPTIMIZER_H

#include "retdec/llvmir2hll/analysis/value_analysis.h"
#include "retdec/llvmir2hll/analysis/var_uses_visitor.h"
#include "retdec/llvmir2hll/optimizer/func_optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class DeadLocalAssignCallOptimizer : public FuncOptimizer {
public:
    DeadLocalAssignCallOptimizer(ShPtr<Module> module, ShPtr<ValueAnalysis> va);

    std::string getId() const override { return "DeadLocalAssignCall"; }

    void doOptimization() override;

private:
    void runOnFunction(ShPtr<Function> func) override;
    bool tryDemoteCallResults(ShPtr<Function> func);

    ShPtr<ValueAnalysis>  va;
    ShPtr<VarUsesVisitor> vuv;
};

} // namespace llvmir2hll
} // namespace retdec

#endif
