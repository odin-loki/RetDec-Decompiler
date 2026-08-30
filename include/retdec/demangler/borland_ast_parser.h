/**
 * @file include/retdec/demangler/borland_ast_parser.h
 * @brief Parser of mangled names into tree for borland demangler.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_BORLAND_AST_PARSER_H
#define RETDEC_BORLAND_AST_PARSER_H

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>

#include "retdec/demangler/context.h"
#include "retdec/demangler/borland_ast/node.h"

namespace retdec {
namespace demangler {
namespace borland {

/// RetDec-owned view with the LLVM 8 itanium `StringView` operations the
/// Borland parser uses. LLVM 23 demangler uses `std::string_view` only.
class StringView
{
	public:
		StringView() = default;
		StringView(const char* s) : v(s ? std::string_view(s) : std::string_view()) {}
		StringView(const char* s, std::size_t n) : v(s, n) {}
		StringView(const char* first, const char* last)
			: v(first, static_cast<std::size_t>(last - first)) {}

		const char* begin() const { return v.data(); }
		const char* end() const { return v.data() + v.size(); }
		std::size_t size() const { return v.size(); }
		bool empty() const { return v.empty(); }
		char front() const { return v.front(); }

		bool startsWith(char c) const { return !v.empty() && v.front() == c; }
		bool startsWith(StringView s) const
		{
			return v.size() >= s.size() && v.substr(0, s.size()) == s.v;
		}

		bool consumeFront(char c)
		{
			if (!startsWith(c))
			{
				return false;
			}
			v.remove_prefix(1);
			return true;
		}
		bool consumeFront(const char* s) { return consumeFront(StringView(s)); }
		bool consumeFront(StringView s)
		{
			if (!startsWith(s))
			{
				return false;
			}
			v.remove_prefix(s.size());
			return true;
		}

		char popFront()
		{
			char c = front();
			v.remove_prefix(1);
			return c;
		}

		void drop(std::size_t n) { v.remove_prefix(std::min(n, v.size())); }

		StringView cutUntil(char c)
		{
			std::size_t pos = 0;
			while (pos < v.size() && v[pos] != c)
			{
				++pos;
			}
			StringView prefix(v.data(), pos);
			v.remove_prefix(pos);
			return prefix;
		}

	private:
		std::string_view v;
};

class FunctionTypeNode;
class NodeArray;
enum class CallConv;

/**
 * @brief Parses name mangled by borland mangling scheme into AST.
 */
class BorlandASTParser
{
public:
	enum Status : uint8_t
	{
		success = 0,
		init,
		in_progress,
		invalid_mangled_name,
		unknown_error,
	};

public:
	explicit BorlandASTParser(Context &context);

	void parse(const std::string &mangled);

	std::shared_ptr<Node> ast();

	Status status();

private:
	char peek() const;
	bool peek(char c) const;
	bool peek(const StringView &s) const;
	unsigned peekNumber() const;
	bool statusOk() const;
	bool checkResult(std::shared_ptr<Node> node);
	bool consumeIfPossible(char c);
	bool consumeIfPossible(const StringView &s);
	bool consume(char c);
	bool consume(const StringView &s);

	std::shared_ptr<Node> parseFunction();
	std::shared_ptr<FunctionTypeNode> parseFuncType(Qualifiers &quals);
	Qualifiers parseQualifiers();
	CallConv parseCallConv();
	std::shared_ptr<NodeArray> parseFuncParams();
	bool parseBackref(std::shared_ptr<NodeArray> &paramArray);
	std::shared_ptr<TypeNode> parseType();
	std::shared_ptr<TypeNode> parseBuildInType(const Qualifiers &quals);
	unsigned parseNumber();
	std::shared_ptr<TypeNode> parseNamedType(unsigned nameLen, const Qualifiers &quals);
	std::shared_ptr<Node> parseFuncName();
	std::shared_ptr<Node> parseFuncNameClasic();
	std::shared_ptr<Node> parseFuncNameLlvm();
	bool couldBeOperator();
	std::shared_ptr<Node> parseOperator();
	std::shared_ptr<Node> parseAsNameUntil(const char *end);
	std::shared_ptr<Node> parseTemplate(std::shared_ptr<Node> templateNamespace);
	std::shared_ptr<Node> parseTemplateName(std::shared_ptr<Node> templateNamespace);
	std::shared_ptr<Node> parseTemplateParams();
	std::shared_ptr<TypeNode> parsePointer(const Qualifiers &quals);
	std::shared_ptr<TypeNode> parseReference();
	std::shared_ptr<TypeNode> parseRReference();
	std::shared_ptr<TypeNode> parseArray(const Qualifiers &quals);
	std::shared_ptr<Node> parseIntExpresion(StringView &s);
	unsigned parseNumber(StringView &s);
	bool parseTemplateBackref(
		StringView &mangled,
		std::shared_ptr<NodeArray> &params);
private:
	Status _status;
	StringView _mangled;
	std::shared_ptr<Node> _ast;
	Context &_context;
};

}    // borland
}    // demangler
}    // retdec

#endif //RETDEC_BORLAND_AST_PARSER_H
