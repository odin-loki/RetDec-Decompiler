/**
 * @file src/demangler/itanium_demangler_adapter.cpp
 * @brief Implementation of itanium demangler adapter.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

#include <llvm/Demangle/Demangle.h>
#include <llvm/Demangle/ItaniumDemangle.h>

#include "retdec/demangler/itanium_ast_ctypes_parser.h"
#include "retdec/demangler/itanium_demangler.h"

namespace retdec {
namespace demangler {

namespace {

class RetDecItaniumAlloc
{
public:
	void reset()
	{
		for (void* p : owned)
		{
			std::free(p);
		}
		owned.clear();
	}

	~RetDecItaniumAlloc() { reset(); }

	template<typename T, typename... Args>
	T* makeNode(Args&&... args)
	{
		void* mem = std::malloc(sizeof(T));
		if (!mem)
		{
			std::abort();
		}
		owned.push_back(mem);
		return new (mem) T(std::forward<Args>(args)...);
	}

	void* allocateNodeArray(size_t sz)
	{
		void* mem = std::malloc(sizeof(llvm::itanium_demangle::Node*) * sz);
		if (!mem)
		{
			std::abort();
		}
		owned.push_back(mem);
		return mem;
	}

private:
	std::vector<void*> owned;
};

} // anonymous namespace

/**
 * @brief Constructor for adapter.
 */
ItaniumDemangler::ItaniumDemangler() : Demangler("itanium") {}

/**
 * @brief Method for demangling to string. After use demangler status should be checked.
 * @param mangled Name mangled by itanium mangling scheme.
 * @return Demangled name.
 */
std::string ItaniumDemangler::demangleToString(const std::string &mangled)
{
	std::string demangled_str = "";

	char *demangled_c = llvm::itaniumDemangle(mangled);
	if (demangled_c)
	{
		_status = success;
		demangled_str = demangled_c;
		std::free(demangled_c);
	}
	else
	{
		_status = invalid_mangled_name;
	}

	return demangled_str;
}

std::shared_ptr<ctypes::Function> ItaniumDemangler::demangleFunctionToCtypes(
	const std::string &mangled,
	std::unique_ptr<ctypes::Module> &module,
	const ctypesparser::CTypesParser::TypeWidths &typeWidths,
	const ctypesparser::CTypesParser::TypeSignedness &typeSignedness,
	unsigned defaultBitWidth)
{
	using DemanglerParser = llvm::itanium_demangle::ManglingParser<RetDecItaniumAlloc>;

	DemanglerParser Parser(mangled.c_str(), mangled.c_str() + mangled.size());

	llvm::itanium_demangle::Node *AST = Parser.parse();
	if (AST == nullptr) {
		_status = invalid_mangled_name;
		return nullptr;
	}

	ItaniumAstCtypesParser ctypesParser;
	auto func = ctypesParser.parseAsFunction(
			mangled,
			AST,
			module,
			typeWidths,
			typeSignedness,
			defaultBitWidth);
	if (func) {
		_status = success;
	}
	return func;
}

}
}
