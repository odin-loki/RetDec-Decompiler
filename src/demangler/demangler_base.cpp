/**
 * @file src/demangler/demangler_base.cpp
 * @brief Demangler library implementation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/demangler/demangler_base.h"

namespace retdec {
namespace demangler {

/**
 * Abstract constructor.
 * @param compiler Name of compiler mangling scheme.
 */
Demangler::Demangler(const std::string &compiler) :
	_compiler(compiler), _status(init) {}

/**
 * @return Currend demangler status.
 */
Demangler::Status Demangler::status()
{
	return _status;
}

}
}
