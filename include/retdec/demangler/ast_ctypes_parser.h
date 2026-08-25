/**
* @file include/retdec/demangler/ast_ctypes_parser.h
* @brief Base class for all AST to ctypes parsers.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#ifndef RETDEC_AST_CTYPES_PARSER_H
#define RETDEC_AST_CTYPES_PARSER_H

#include "retdec/ctypesparser/ctypes_parser.h"

namespace retdec {
namespace demangler {

/*
 * Base class for all AST to ctypes parsers.
 */
class AstToCtypesParser: public retdec::ctypesparser::CTypesParser {
public:
	AstToCtypesParser () = default;

protected:
	unsigned getBitWidth(const std::string &typeName) const;

	ctypes::IntegralType::Signess toSigness(bool isUnsigned) const;

	ctypes::IntegralType::Signess toSigness(const std::string &typeName) const;

	ctypes::FunctionType::VarArgness toVarArgness(bool isVarArg) const;
};

} // demangler
} // retdec

#endif //RETDEC_AST_CTYPES_PARSER_H
