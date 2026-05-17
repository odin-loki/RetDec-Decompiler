/**
* @file src/ctypes/function_declaration.cpp
* @brief Implementation of FunctionDeclaration.
* @copyright (c) 2017 Odin Loch Trading as Imortek
*/

#include "retdec/ctypes/function_declaration.h"

namespace retdec {
namespace ctypes {

/**
* @brief Constructs a new function declaration.
*/
FunctionDeclaration::FunctionDeclaration(const std::string &declaration):
	declaration(declaration) {}

FunctionDeclaration::operator std::string() const
{
	return declaration;
}

} // namespace ctypes
} // namespace retdec
