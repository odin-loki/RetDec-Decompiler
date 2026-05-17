/**
* @file include/retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_symbolic_names_for_param.h
* @brief Provides getSymbolicNamesForParam() for GCCGeneralSemantics.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_GCC_GENERAL_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H
#define RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_GCC_GENERAL_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H

#include <optional>
#include <string>

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace gcc_general {

std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
    unsigned paramPos);

} // namespace gcc_general
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec

#endif
