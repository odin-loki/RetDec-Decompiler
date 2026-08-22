/**
* @file include/retdec/llvmir2hll/semantics/semantics/libc_semantics/get_symbolic_names_for_param.h
* @brief Provides getSymbolicNamesForParam() for LibcSemantics.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_LIBC_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H
#define RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_LIBC_SEMANTICS_GET_SYMBOLIC_NAMES_FOR_PARAM_H

#include <optional>
#include <string>

#include "retdec/llvmir2hll/semantics/semantics/impl_support/get_symbolic_names_for_param.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace libc {

std::optional<IntStringMap> getSymbolicNamesForParam(const std::string &funcName,
    unsigned paramPos);

} // namespace libc
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec

#endif
