/**
* @file include/retdec/llvmir2hll/semantics/semantics/libc_semantics/get_arity_of_func.h
* @brief Declaration of semantics::libc::getArityOfFunc() for LibcSemantics.
* @copyright (c) 2025-2026 Odin Loch trading as Imortek
*/

#ifndef RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_LIBC_SEMANTICS_GET_ARITY_OF_FUNC_H
#define RETDEC_LLVMIR2HLL_SEMANTICS_SEMANTICS_LIBC_SEMANTICS_GET_ARITY_OF_FUNC_H

#include <optional>
#include <string>
#include <unordered_map>

#include "retdec/llvmir2hll/semantics/semantics.h"

namespace retdec {
namespace llvmir2hll {
namespace semantics {
namespace libc {

/// Mapping of a function name to its declared arity.
using FuncArityMap = std::unordered_map<std::string, FuncArity>;

/**
* @brief Implements getArityOfFunc() for LibcSemantics.
*
* See Semantics::getArityOfFunc() for more details.
*/
std::optional<FuncArity> getArityOfFunc(const std::string &funcName);

} // namespace libc
} // namespace semantics
} // namespace llvmir2hll
} // namespace retdec

#endif
