/**
* @file src/ctypes/call_convention.cpp
* @brief Implementation of CallConvention.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include "retdec/ctypes/call_convention.h"

namespace retdec {
namespace ctypes {

/**
* @brief Constructs a new call convention.
*/
CallConvention::CallConvention(const std::string &callConvention):
	callConvention(callConvention) {}

CallConvention::operator std::string() const
{
	return callConvention;
}

bool CallConvention::operator==(const CallConvention &other) const
{
	return callConvention == other.callConvention;
}

bool CallConvention::operator!=(const CallConvention &other) const
{
	return !(*this == other);
}

} // namespace ctypes
} // namespace retdec
