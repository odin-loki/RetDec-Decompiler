/**
* @file include/retdec/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer.h
* @brief Promotes constant byte arrays that look like strings to ConstString.
* @copyright (c) 2024, MIT license
*/

#ifndef RETDEC_LLVMIR2HLL_OPTIMIZER_CHAR_ARRAY_TO_STRING_OPTIMIZER_H
#define RETDEC_LLVMIR2HLL_OPTIMIZER_CHAR_ARRAY_TO_STRING_OPTIMIZER_H

#include <string>
#include <vector>

#include "retdec/llvmir2hll/ir/const_array.h"
#include "retdec/llvmir2hll/optimizer/optimizer.h"
#include "retdec/llvmir2hll/support/smart_ptr.h"

namespace retdec {
namespace llvmir2hll {

class Module;

class CharArrayToStringOptimizer : public Optimizer {
public:
    explicit CharArrayToStringOptimizer(ShPtr<Module> module);
    virtual std::string getId() const override;

protected:
    virtual void doOptimization() override;

private:
    bool tryExtractString(ShPtr<ConstArray> arr, std::string& out);
};

} // namespace llvmir2hll
} // namespace retdec

#endif
