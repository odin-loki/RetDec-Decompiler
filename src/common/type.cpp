/**
 * @file src/common/type.cpp
 * @brief Common data type representation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cassert>

#include "retdec/common/type.h"

namespace retdec {
namespace common {

/**
 * Default type is i32.
 */
Type::Type()
{
}

Type::Type(const std::string& llvmIrRepre) :
		_llvmIr(llvmIrRepre)
{
}

/**
 * @return Type is defined if @c llvmIr member is not empty.
 */
bool Type::isDefined() const
{
	return !_llvmIr.empty();
}

/**
 * Wide strings are in LLVM IR represented as int arrays.
 * This flag can be use to distinguish them from ordinary int arrays.
 */
bool Type::isWideString() const
{
	return _wideString;
}

void Type::setIsWideString(bool b)
{
	_wideString = b;
}

void Type::setLlvmIr(const std::string& t)
{
	_llvmIr = t;
}

/**
 * @return Type's ID is its LLVM IR representation.
 *
 * Reads the member rather than calling getLlvmIr(), which asserts the type is
 * defined. This is a container key, and an undefined type reaches a container
 * straight from a config file: `{"structures": [{}]}`.
 */
std::string Type::getId() const
{
	return _llvmIr;
}

/**
 * @return LLVM IR string representation (unique ID).
 */
std::string Type::getLlvmIr() const
{
	assert(isDefined());
	return _llvmIr;
}

/**
 * Less-than comparison of this instance with the provided one.
 * Default string comparison of @c llvmIr members is used.
 * @param val Other type to compare with.
 * @return True if @c this instance is considered to be less-than @c val.
 */
bool Type::operator<(const Type& val) const
{
	// No assert, and no getLlvmIr(), which has one. An ordering has to be
	// total over every value the type can hold, and an undefined one -- a
	// "structures" entry in a config file with no "llvmIr" -- is one of them.
	// It sorted after nothing and aborted the process instead, on the second
	// structure inserted. The empty string sorts before every other, which is
	// where an undefined type belongs.
	return _llvmIr < val._llvmIr;
}

/**
 * Types are equal if their llvm ir representations are equal.
 */
bool Type::operator==(const Type& val) const
{
	return _llvmIr == val._llvmIr;
}

} // namespace common
} // namespace retdec
