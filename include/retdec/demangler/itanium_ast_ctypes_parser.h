/**
* @file include/retdec/demangler/itanium_ast_ctypes_parser.h
* @brief Parser for AST created in Itanium demangler to ctypes parsers.
* @copyright (c) 2019 Odin Loch Trading as Imortek
*/

#ifndef RETDEC_ITANIUM_AST_CTYPES_PARSER_H
#define RETDEC_ITANIUM_AST_CTYPES_PARSER_H

#include <memory>
#include "retdec/demangler/ast_ctypes_parser.h"

namespace llvm {
namespace itanium_demangle {
class FunctionEncoding;
class FunctionType;
class NameType;
class NodeArray;
class Node;
class PointerType;
class ReferenceType;
class ArrayType;
class NodeOrString;
}
}

namespace retdec {
namespace demangler {

/*
 * @brief Parser for AST created in Itanium demangler to ctypes parsers.
 */
class ItaniumAstCtypesParser : public AstToCtypesParser
{
public:
	ItaniumAstCtypesParser() = default;

	std::shared_ptr<ctypes::Function> parseAsFunction(
		const std::string &name,
		const llvm::itanium_demangle::Node *ast,
		std::unique_ptr<ctypes::Module> &module,
		const TypeWidths&typeWidths = TypeWidths{},
		const TypeSignedness&typeSignedness = TypeSignedness{},
		unsigned defaultBitWidth = 0);

private:
	std::shared_ptr<ctypes::Function> parseFunction(
		const std::string &mangledName, const llvm::itanium_demangle::FunctionEncoding *functionEncodingNode);

	ctypes::Function::Parameters parseFunctionParameters(
		llvm::itanium_demangle::NodeArray node, bool &isVarArg);

	std::shared_ptr<ctypes::Type> parseType(
		const llvm::itanium_demangle::Node *typeNode);

	std::shared_ptr<ctypes::IntegralType> parseIntegralType(
		const std::string &name);

	std::shared_ptr<ctypes::FloatingPointType> parseFloatingPointType(
		const std::string &name);

	std::shared_ptr<ctypes::PointerType> parsePointer(
		const llvm::itanium_demangle::PointerType *typeNode);

	std::shared_ptr<ctypes::Type> parseReference(
		const llvm::itanium_demangle::ReferenceType *typeNode);

	std::shared_ptr<ctypes::Type> parseNameTypeNode(
		const llvm::itanium_demangle::NameType *typeNode);

	std::shared_ptr<ctypes::ArrayType> parseArrayType(
		const llvm::itanium_demangle::ArrayType *typeNode);

	unsigned parseDimension(
		const llvm::itanium_demangle::NodeOrString *dimensions);

	std::shared_ptr<ctypes::FunctionType> parseFuntionType(
		const llvm::itanium_demangle::FunctionType *typeNode);

	ctypes::FunctionType::Parameters parseFuncTypeParameters(
		llvm::itanium_demangle::NodeArray parameters,
		bool &isVarArg);
};

} // demangler
} // retdec

#endif //RETDEC_ITANIUM_AST_CTYPES_PARSER_H
