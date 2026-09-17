/**
* @file src/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer.cpp
* @brief Promote constant byte arrays that look like strings to ConstString.
* @copyright (c) 2024, MIT license
*
* The existing EmptyArrayToStringOptimizer only handles arrays of empty
* strings. This pass handles the more common case: a global or local array
* of i8/i32 constants that contains only printable ASCII + null terminator,
* such as:
*
*   int8_t g_str[] = {72, 101, 108, 108, 111, 0};  // "Hello"
*
* → replaced with:
*
*   char *g_str = "Hello";
*
* Conditions for promotion:
*  1. The array is initialized (not extern).
*  2. Every element is a ConstInt in [0, 127].
*  3. At least 50% of the elements (excluding the null terminator) are
*     printable ASCII (32–126).
*  4. The array ends with a zero byte (null-terminated).
*  5. The array length is between 2 and 4096 bytes.
*/

#include <cctype>
#include <string>

#include "retdec/llvmir2hll/ir/const_array.h"
#include "retdec/llvmir2hll/ir/const_int.h"
#include "retdec/llvmir2hll/ir/const_string.h"
#include "retdec/llvmir2hll/ir/array_type.h"
#include "retdec/llvmir2hll/ir/global_var_def.h"
#include "retdec/llvmir2hll/ir/int_type.h"
#include "retdec/llvmir2hll/ir/module.h"
#include "retdec/llvmir2hll/optimizer/optimizers/char_array_to_string_optimizer.h"
#include "retdec/llvmir2hll/support/debug.h"

namespace retdec {
namespace llvmir2hll {

CharArrayToStringOptimizer::CharArrayToStringOptimizer(ShPtr<Module> module)
    : Optimizer(module) {
    PRECONDITION_NON_NULL(module);
}

void CharArrayToStringOptimizer::doOptimization() {
    for (auto i = module->global_var_begin(), e = module->global_var_end();
            i != e; ++i) {
        ShPtr<ConstArray> arr(cast<ConstArray>((*i)->getInitializer()));
        if (!arr || !arr->isInitialized()) continue;

        std::string str;
        if (!tryExtractString(arr, str)) continue;

        (*i)->setInitializer(ConstString::create(str));
    }
}

bool CharArrayToStringOptimizer::tryExtractString(ShPtr<ConstArray> arr,
                                                   std::string& out) {
    if (!arr->isInitialized()) return false;

	// A ConstString is bytes. Promoting an int16_t[] or int32_t[] table whose
	// elements all happen to be printable ASCII prints
	// `int32_t g[6] = "Hello";` -- not valid C, and describing six bytes where
	// the binary has twenty-four. Only an 8-bit element array is a string.
	ShPtr<ArrayType> arrTy(cast<ArrayType>(arr->getType()));
	if (!arrTy) return false;
	ShPtr<IntType> elemTy(cast<IntType>(arrTy->getContainedType()));
	if (!elemTy || elemTy->getSize() != 8) return false;

	std::size_t n = 0;
    // Count elements.
    for (auto it = arr->init_begin(), e = arr->init_end(); it != e; ++it)
        ++n;

    if (n < 2 || n > 4096) return false;

    // Collect bytes.
    std::vector<int> bytes;
    bytes.reserve(n);
    for (auto it = arr->init_begin(), e = arr->init_end(); it != e; ++it) {
        ShPtr<ConstInt> ci(cast<ConstInt>(*it));
        if (!ci) return false;
		const llvm::APInt& v = ci->getValue();
		if (v.getActiveBits() > 7) return false; // also excludes negatives
		int val = static_cast<int>(v.getZExtValue());
		bytes.push_back(val);
    }

    // Must end with null terminator.
    if (bytes.back() != 0) return false;

    // At least 50% printable ASCII (excluding the null).
    std::size_t printable = 0;
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i)
        if (bytes[i] >= 32 && bytes[i] <= 126) ++printable;

    if (bytes.size() <= 1) return false;
    if (printable * 2 < (bytes.size() - 1)) return false;

    // Build the string (exclude null terminator).
    out.clear();
    for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
        out += static_cast<char>(bytes[i]);
    }
    return true;
}

std::string CharArrayToStringOptimizer::getId() const {
    return "CharArrayToString";
}

} // namespace llvmir2hll
} // namespace retdec
