/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/unknown_type_inferrer.h
* @brief Infer concrete types for variables whose type is UnknownType.
*
* Uses constraint propagation over the HLL IR:
*  - assignment sources propagate their type to UnknownType targets
*  - dereference context implies pointer type
*  - array-index base implies pointer type
*  - null-pointer comparisons imply pointer type
*  - known function call-site arguments imply the matching parameter type
*  - fallback: void *
*
* Runs to a fixed point (multiple passes until no more types can be inferred).
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_UNKNOWN_TYPE_INFERRER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_UNKNOWN_TYPE_INFERRER_H

#include "retdec/llvmir2hll/optimizer/func_optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class Module;

/**
* @brief Replaces UnknownType on variables with inferred concrete types.
*
* Instances of this class have reference object semantics.
*/
class UnknownTypeInferrer final : public FuncOptimizer {
public:
    explicit UnknownTypeInferrer(ShPtr<Module> module);

    virtual std::string getId() const override { return "UnknownTypeInferrer"; }

protected:
    virtual void doOptimization() override;
};

} // namespace llvmir2hll
} // namespace retdec

#endif // RETDEC_LLVMIR2HLL_OPTIMIZER_OPTIMIZERS_UNKNOWN_TYPE_INFERRER_H
