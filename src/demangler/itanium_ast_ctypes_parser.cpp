/**
* @file src/demangler/itanium_ast_ctypes_parser.cpp
* @brief Parser from AST created by Itanium demangler to C-types.
* @copyright (c) 2017 Avast Software, licensed under the MIT license
* @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
*/

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <llvm/Demangle/ItaniumDemangle.h>
#include <llvm/Demangle/Utility.h>

#include "retdec/demangler/itanium_ast_ctypes_parser.h"

namespace retdec {
namespace demangler {

using Kind = llvm::itanium_demangle::Node::Kind;

namespace {

/*
 * @return String representation of node and it's subtree.
 */
std::string toString(const llvm::itanium_demangle::Node *node)
{
	if (!node) {
		return {};
	}

	llvm::itanium_demangle::OutputBuffer s;
	node->print(s);
	std::string name(static_cast<std::string_view>(s));
	std::free(s.getBuffer());
	return name;
}

}    // anonymous namespace

/*
 * @brief Entry point for parsing AST representation of functions into ctypes functions.
 * Sets internal variables and checks if root AST node is function.
 */
std::shared_ptr<ctypes::Function> ItaniumAstCtypesParser::parseAsFunction(
	const std::string &name,
	const llvm::itanium_demangle::Node *ast,
	std::unique_ptr<ctypes::Module> &module,
	const CTypesParser::TypeWidths &typeWidths,
	const CTypesParser::TypeSignedness &typeSignedness,
	unsigned defaultBitWidth)
{
	assert(ast && "Ast cannot be null");

	this->context = module->getContext();
	this->typeWidths = typeWidths;
	this->typeSignedness = typeSignedness;
	this->defaultBitWidth = defaultBitWidth;

	if (ast->getKind() == Kind::KFunctionEncoding) {
		auto func = parseFunction(name, static_cast<const llvm::itanium_demangle::FunctionEncoding *>(ast));
		if (func) {
			module->addFunction(func);
			return func;
		}
	}

	return nullptr;
}

std::shared_ptr<ctypes::Function> ItaniumAstCtypesParser::parseFunction(
	const std::string &mangledName,
	const llvm::itanium_demangle::FunctionEncoding *functionEncodingNode)
{
	assert(functionEncodingNode && "Violated precondition.");

	std::shared_ptr<ctypes::Type> returnType = parseType(functionEncodingNode->getReturnType());
	bool isVarArg = false;
	ctypes::Function::Parameters parameters =
		parseFunctionParameters(functionEncodingNode->getParams(), isVarArg);
	ctypes::Function::VarArgness varArgness = toVarArgness(isVarArg);
	ctypes::CallConvention callConvention = ctypes::CallConvention("unknown");    // calling convention is not mangled in names

	auto func = ctypes::Function::create(context, mangledName, returnType, parameters, callConvention, varArgness);
	auto declaration = toString(functionEncodingNode);
	if (func && !declaration.empty()) {
		func->setDeclaration(ctypes::FunctionDeclaration(declaration));
	}
	return func;
}

ctypes::Function::Parameters ItaniumAstCtypesParser::parseFunctionParameters(
	llvm::itanium_demangle::NodeArray node,
	bool &isVarArg)
{
	using Parameters = ctypes::Function::Parameters;

	Parameters parameters;

	for (size_t i = 0; i < node.size(); ++i) {
		auto type = parseType(node[i]);
		if (type->isNamed()
			&& std::static_pointer_cast<ctypes::NamedType>(type)->getName() == "...") {
			isVarArg = true;
		} else {
			auto parameter = ctypes::Parameter("param" + std::to_string(i), type);
			parameters.emplace_back(parameter);
		}
	}

	return parameters;
}

std::shared_ptr<ctypes::Type> ItaniumAstCtypesParser::parseType(
	const llvm::itanium_demangle::Node *typeNode)
{
	if (!typeNode) {
		return ctypes::UnknownType::create();	// shouldn't happen
	}

	std::shared_ptr<ctypes::Type> parsedType;

	switch (typeNode->getKind()) {
	case Kind::KQualType:
		parsedType = parseType(static_cast<const llvm::itanium_demangle::QualType *>(typeNode)->getChild());
		break;
	case Kind::KPointerType:
		parsedType = parsePointer(static_cast<const llvm::itanium_demangle::PointerType *>(typeNode));
		break;
	case Kind::KReferenceType:
		parsedType = parseReference(static_cast<const llvm::itanium_demangle::ReferenceType *>(typeNode));
		break;
	case Kind::KNameType:
		parsedType = parseNameTypeNode(static_cast<const llvm::itanium_demangle::NameType *>(typeNode));
		break;
	case Kind::KArrayType:
		parsedType = parseArrayType(static_cast<const llvm::itanium_demangle::ArrayType *>(typeNode));
		break;
	case Kind::KFunctionType:
		parsedType = parseFuntionType(static_cast<const llvm::itanium_demangle::FunctionType *>(typeNode));
		break;
	case Kind::KNameWithTemplateArgs:
		parsedType = ctypes::NamedType::create(context, toString(typeNode));
		break;
	default:
		parsedType = ctypes::UnknownType::create();
	}

	return parsedType;
}

std::shared_ptr<ctypes::IntegralType> ItaniumAstCtypesParser::parseIntegralType(const std::string &name)
{
	assert(!name.empty() && "Violated precondition.");

	unsigned bitWidth = getBitWidth(name);
	ctypes::IntegralType::Signess signess = toSigness(name);

	return ctypes::IntegralType::create(context, name, bitWidth, signess);
}

std::shared_ptr<ctypes::FloatingPointType> ItaniumAstCtypesParser::parseFloatingPointType(
	const std::string &name)
{
	assert(!name.empty() && "Violated precondition.");

	unsigned bitWidth = getBitWidth(name);

	return ctypes::FloatingPointType::create(context, name, bitWidth);
}

std::shared_ptr<ctypes::PointerType> ItaniumAstCtypesParser::parsePointer(
	const llvm::itanium_demangle::PointerType *typeNode)
{
	assert(typeNode && "Violated precondition.");

	auto pointee = parseType(typeNode->getPointee());
	unsigned bitWidth = getBitWidth("ptr_t");

	return ctypes::PointerType::create(context, pointee, bitWidth);
}

std::shared_ptr<ctypes::Type> ItaniumAstCtypesParser::parseReference(
	const llvm::itanium_demangle::ReferenceType *typeNode)
{
	assert(typeNode && "Violated precondition.");

	const llvm::itanium_demangle::Node *pointeeNode = nullptr;
	typeNode->match([&](const llvm::itanium_demangle::Node *p,
			llvm::itanium_demangle::ReferenceKind) {
		pointeeNode = p;
	});
	auto pointee = parseType(pointeeNode);
	unsigned bitWidth = getBitWidth("ptr_t");

	return ctypes::ReferenceType::create(context, pointee, bitWidth);	// both LValue and RValue
}

/*
 * @brief Parses types based on their name.
 * Itanium AST uses KNameType nodes for representation of types, hence the name.
 * It parses not only ctypes::NamedType
 * To make parsing easy, information about varArgness is returned as named type.
 * This type should't be stored anywhere and varArgness of function should be set.
 */
std::shared_ptr<ctypes::Type> ItaniumAstCtypesParser::parseNameTypeNode(
	const llvm::itanium_demangle::NameType *typeNode)
{
	assert(typeNode && "Violated precondition.");

	std::string name = std::string(typeNode->getName());

	if (name == "void") {
		return ctypes::VoidType::create();
	}

	if (name == "wchar_t"
		|| name == "bool"
		|| name == "char"
		|| name == "signed char"
		|| name == "unsigned char"
		|| name == "char32_t"
		|| name == "char16_t"
		|| name == "short"
		|| name == "unsigned short"
		|| name == "int"
		|| name == "unsigned int"
		|| name == "long"
		|| name == "unsigned long"
		|| name == "long long"
		|| name == "unsigned long long"
		|| name == "__int128"
		|| name == "unsigned __int128") {
		return parseIntegralType(name);
	}

	if (name == "float"
		|| name == "double"
		|| name == "long double"
		|| name == "__float128"
		|| name == "decimal64"
		|| name == "decimal128"
		|| name == "decimal32"
		|| name == "decimal16"
		|| name == "half") {
		return parseFloatingPointType(name);
	}

	if (name == "auto"
		|| name == "decltype(auto)"
		|| name == "std::nullptr_t") {
		return ctypes::UnknownType::create();
	}

	/*
	 * if (name == "...") return type with name "..."; <-- is implicit
	 * is later used to check if function takes variable number of arguments
	 */
	return ctypes::NamedType::create(context, name);
}

std::shared_ptr<ctypes::ArrayType> ItaniumAstCtypesParser::parseArrayType(
	const llvm::itanium_demangle::ArrayType *typeNode)
{
	using Dimensions = ctypes::ArrayType::Dimensions;

	Dimensions dimensions;

	const llvm::itanium_demangle::Node *base = nullptr;
	llvm::itanium_demangle::Node *dim = nullptr;
	typeNode->match([&](const llvm::itanium_demangle::Node *b,
			llvm::itanium_demangle::Node *d) {
		base = b;
		dim = d;
	});
	dimensions.emplace_back(parseDimension(dim));

	while (base && base->getKind() == Kind::KArrayType) {
		auto nestedArrayType = static_cast<const llvm::itanium_demangle::ArrayType *>(base);
		nestedArrayType->match([&](const llvm::itanium_demangle::Node *b,
				llvm::itanium_demangle::Node *d) {
			base = b;
			dim = d;
		});
		dimensions.emplace_back(parseDimension(dim));
	}

	auto type = parseType(base);

	return ctypes::ArrayType::create(context, type, dimensions);
}

unsigned ItaniumAstCtypesParser::parseDimension(const llvm::itanium_demangle::Node *dimensions)
{
	if (!dimensions) {
		return 0;
	}

	std::string dimStr = toString(dimensions);

	unsigned dim = 0;
	try {
		size_t lenParsed;
		int parsed = std::stoi(dimStr, &lenParsed);	// catch exception if fails
		if (lenParsed == dimStr.length()) {
			dim = static_cast<unsigned>(parsed);
		}
	} catch (std::exception &e) {
		dim = 0;
	}

	return dim;
}

std::shared_ptr<ctypes::FunctionType> ItaniumAstCtypesParser::parseFuntionType(
	const llvm::itanium_demangle::FunctionType *typeNode)
{
	const llvm::itanium_demangle::Node *ret = nullptr;
	llvm::itanium_demangle::NodeArray params;
	typeNode->match([&](const llvm::itanium_demangle::Node *r,
			llvm::itanium_demangle::NodeArray p,
			llvm::itanium_demangle::Qualifiers,
			llvm::itanium_demangle::FunctionRefQual,
			const llvm::itanium_demangle::Node *) {
		ret = r;
		params = p;
	});

	std::shared_ptr<ctypes::Type> returnType = parseType(ret);
	bool isVarArg = false;
	ctypes::FunctionType::Parameters parameters =
		parseFuncTypeParameters(params, isVarArg);
	ctypes::Function::VarArgness varArgness = toVarArgness(isVarArg);
	ctypes::CallConvention callConvention = ctypes::CallConvention("unknown");	// itanium scheme never mangles call conv

	return ctypes::FunctionType::create(context, returnType, parameters, callConvention, varArgness);
}

ctypes::FunctionType::Parameters ItaniumAstCtypesParser::parseFuncTypeParameters(
	const llvm::itanium_demangle::NodeArray parameters_node,
	bool &isVarArg)
{
	using Parameters = ctypes::FunctionType::Parameters;

	Parameters parameters;

	for (size_t i = 0; i < parameters_node.size(); ++i) {
		auto type = parseType(parameters_node[i]);
		if (type->isNamed()
			&& std::static_pointer_cast<ctypes::NamedType>(type)->getName() == "...") {
			// to make parsing easy, information about varArgness is temporarely passed as named type
			// this type should't be stored anywhere
			isVarArg = true;
		} else {
			parameters.emplace_back(type);
		}
	}

	return parameters;
}

} // demangler
} // retdec
