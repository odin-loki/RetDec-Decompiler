/**
* @file include/retdec/llvmir2hll/semantics/semantics/macos_semantics/get_symbolic_names_for_param.h
* @brief Provides getSymbolicNamesForParam() for macOS/BSD targets.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_MACOS_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H
#define RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_MACOS_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H

#include <optional>
#include <string>

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace macos {

std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
    unsigned paramPos);

} // namespace macos
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec

#endif
