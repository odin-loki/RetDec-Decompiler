/**
* @file src/llvmir2hll/semantics/semantics/gcc_general_semantics.cpp
* @brief Implementation of GCCGeneralSemantics.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics.h"
#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_arity_of_func.h"
#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_c_header_file_for_func.h"
#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_name_of_param.h"
#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_name_of_var_storing_result.h"
#include "retdec/llvmir2hll/semantics/semantics/gcc_general_semantics/get_symbolic_names_for_param.h"
#include "retdec/llvmir2hll/semantics/semantics_factory.h"
#include "retdec/llvmir2hll/support/debug.h"
#include "retdec/llvmir2hll/support/types.h"

namespace retdec {
namespace llvmir2hll {

REGISTER_AT_FACTORY("gcc-general", GCC_GENERAL_SEMANTICS_ID, SemanticsFactory,
	GCCGeneralSemantics::create);

/**
* @brief Constructs the semantics.
*/
GCCGeneralSemantics::GCCGeneralSemantics() {}

/**
* @brief Creates a new semantics.
*/
ShPtr<Semantics> GCCGeneralSemantics::create() {
	return ShPtr<Semantics>(new GCCGeneralSemantics());
}

std::string GCCGeneralSemantics::getId() const {
	return GCC_GENERAL_SEMANTICS_ID;
}

std::optional<std::string> GCCGeneralSemantics::getCHeaderFileForFunc(
		const std::string &funcName) const {
	return semantics::gcc_general::getCHeaderFileForFunc(funcName);
}

std::optional<std::string> GCCGeneralSemantics::getNameOfVarStoringResult(
		const std::string &funcName) const {
	return semantics::gcc_general::getNameOfVarStoringResult(funcName);
}

std::optional<std::string> GCCGeneralSemantics::getNameOfParam(
		const std::string &funcName, unsigned paramPos) const {
	return semantics::gcc_general::getNameOfParam(funcName, paramPos);
}

std::optional<IntStringMap> GCCGeneralSemantics::getSymbolicNamesForParam(
		const std::string &funcName, unsigned paramPos) const {
	return semantics::gcc_general::getSymbolicNamesForParam(funcName, paramPos);
}

std::optional<FuncArity> GCCGeneralSemantics::getArityOfFunc(const std::string& funcName) const
{
	// This used to return nullopt, on the grounds that "GCC's general semantics
	// describes naming and return behaviour, not signatures". The header table
	// beside it is a signature fact: it is what puts `#include <pthread.h>` in
	// the emitted C, and having done that it has committed the output to
	// whatever <pthread.h> declares. CC-01 found the gap -- the corpus emitted
	// `pthread_create(thread)` under a header declaring four parameters -- and
	// libc semantics could not fill it, because pthread is not in its table.
	//
	// So the 976 entries here cover exactly the names this semantics assigns a
	// header to, measured from those headers by the same script that measures
	// the libc table.
	return semantics::gcc_general::getArityOfFunc(funcName);
}

} // namespace llvmir2hll
} // namespace retdec
