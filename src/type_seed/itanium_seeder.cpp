/**
 * @file src/type_seed/itanium_seeder.cpp
 * @brief Itanium C++ ABI mangling parser for type seeding.
 *
 * ## Itanium mangling grammar (subset implemented)
 *
 *   <mangled-name> := _Z <encoding>
 *   <encoding>     := <function-name> <bare-function-type>
 *                   | <data-name>
 *                   | <special-name>
 *   <function-name>:= <nested-name>    (class member)
 *                   | <unscoped-name>  (free function or std:: shorthand)
 *                   | <local-name>
 *   <nested-name>  := N [CV-quals] [ref-qualifier] <prefix>* <unqualified-name> E
 *   <prefix>       := <unqualified-name> | <template-prefix> <template-args>
 *   <unqualified-name> := <operator-name> | <ctor-dtor-name> | <source-name>
 *   <source-name>  := <positive-length> <identifier>
 *   <bare-function-type> := <type>+          (first element = return type for templates)
 *   <type>         := <builtin-type> | <qualified-type> | <function-type>
 *                   | <class-enum-type> | <array-type> | <pointer-to-member-type>
 *                   | <template-param> | <template-template-param> <template-args>
 *                   | <substitution> | P<type> | R<type> | O<type>
 *                   | K<type> (const) | V<type> (volatile) | r<type> (restrict)
 *   <builtin-type> := v(void) i(int) j(unsigned int) l(long) m(unsigned long)
 *                     x(long long) y(unsigned long long) s(short) t(unsigned short)
 *                     c(char) a(signed char) h(unsigned char) w(wchar_t) b(bool)
 *                     f(float) d(double) e(long double) z(...)
 *                     Dn(decltype(nullptr)) Di(char32_t) Ds(char16_t) Da(auto)
 *   <template-args>:= I <template-arg>+ E
 *   <template-arg> := <type> | X<expression>E | <expr-primary>
 *
 * We parse enough of this grammar to extract:
 *   - Class name (if member function)
 *   - Function name
 *   - All parameter types
 *   - Return type (for template functions — it is the FIRST type in
 *     bare-function-type; for non-template functions it is absent)
 *   - CV-qualifiers on `this` (const member → isConst)
 *   - Whether it is a constructor/destructor/operator
 *   - Template arguments for well-known STL types
 */

#include <cstddef>
#include <memory>
#include "retdec/type_seed/type_seed.h"

#ifdef RETDEC_USE_CXXABI_DEMANGLE
#include <cxxabi.h>
#endif

#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace retdec {
namespace type_seed {

// ─── Itanium parser state ─────────────────────────────────────────────────────

namespace {

struct ItaniumParser
{
	const char* p;
	const char* end;
	bool ok = true;

	/// parseType() recurses for each of P R O U A, one level per input byte, so
	/// a symbol that is 200,000 'P's took the stack down with SIGSEGV. Deeper
	/// than this is not a name any compiler emits.
	static constexpr int kMaxDepth = 256;
	int depth = 0;

	struct DepthGuard
	{
		ItaniumParser& parser;
		const bool entered;
		explicit DepthGuard(ItaniumParser& s): parser(s), entered(s.depth < kMaxDepth)
		{
			if (entered) ++parser.depth;
		}
		~DepthGuard()
		{
			if (entered) --parser.depth;
		}
	};

	// Substitution table: S_ S0_ S1_ ...
	std::vector<std::string> subs;

	explicit ItaniumParser(const char* s, std::size_t n): p(s), end(s + n) {}

	bool atEnd() const
	{
		return p >= end;
	}
	char peek() const
	{
		return (p < end) ? *p : '\0';
	}
	char get()
	{
		return (p < end) ? *p++ : '\0';
	}
	bool consume(char c)
	{
		if (peek() == c)
		{
			++p;
			return true;
		}
		return false;
	}
	// Reads a decimal run into a std::size_t, saturating rather than
	// overflowing. `len = len*10 + digit` in an int is undefined behaviour
	// once a symbol offers ten digits, and the compiler is entitled to assume
	// it cannot happen -- which makes the `len <= 0` test that follows
	// unreliable as well as insufficient. libFuzzer reached this in seconds.
	// Returns false when the number ran past the cap, which no symbol table
	// entry can be long enough to need.
	bool readDecimal(std::size_t& out)
	{
		if (!std::isdigit(static_cast<unsigned char>(peek()))) return false;
		constexpr std::size_t kMaxDecimal = 1u << 24;
		std::size_t v = 0;
		bool tooLarge = false;
		while (std::isdigit(static_cast<unsigned char>(peek())))
		{
			const std::size_t d = static_cast<std::size_t>(get() - '0');
			if (v > kMaxDecimal)
				tooLarge = true;
			else
				v = v * 10 + d;
		}
		out = v;
		return !tooLarge;
	}

	bool consume(const char* s)
	{
		std::size_t n = std::strlen(s);
		if (end - p >= (ptrdiff_t)n && std::memcmp(p, s, n) == 0)
		{
			p += n;
			return true;
		}
		return false;
	}

	// ── Builtin type map ──────────────────────────────────────────────────────

	// Returns empty string if not a builtin.
	std::string tryBuiltin()
	{
		static const struct
		{
			char code;
			const char* name;
		} table[] = {
			{'v', "void"},
			{'b', "bool"},
			{'c', "char"},
			{'a', "signed char"},
			{'h', "unsigned char"},
			{'s', "short"},
			{'t', "unsigned short"},
			{'i', "int"},
			{'j', "unsigned int"},
			{'l', "long"},
			{'m', "unsigned long"},
			{'x', "long long"},
			{'y', "unsigned long long"},
			{'n', "__int128"},
			{'o', "unsigned __int128"},
			{'f', "float"},
			{'d', "double"},
			{'e', "long double"},
			{'g', "__float128"},
			{'z', "..."},
			{'w', "wchar_t"},
		};
		for (auto& e: table)
		{
			if (peek() == e.code)
			{
				++p;
				return e.name;
			}
		}
		// Two-character builtins starting with 'D'
		if (peek() == 'D' && p + 1 < end)
		{
			char c2 = p[1];
			if (c2 == 'n')
			{
				p += 2;
				return "decltype(nullptr)";
			}
			if (c2 == 'i')
			{
				p += 2;
				return "char32_t";
			}
			if (c2 == 's')
			{
				p += 2;
				return "char16_t";
			}
			if (c2 == 'u')
			{
				p += 2;
				return "char8_t";
			}
			if (c2 == 'a')
			{
				p += 2;
				return "auto";
			}
			if (c2 == 'f')
			{
				p += 2;
				return "_Decimal32";
			}
			if (c2 == 'd')
			{
				p += 2;
				return "_Decimal64";
			}
			if (c2 == 'e')
			{
				p += 2;
				return "_Decimal128";
			}
		}
		return {};
	}

	// ── Source name (length-prefixed identifier) ──────────────────────────────

	std::string parseSourceName()
	{
		std::size_t len = 0;
		if (!readDecimal(len))
		{
			ok = false;
			return {};
		}
		if (len == 0 || len > static_cast<std::size_t>(end - p))
		{
			ok = false;
			return {};
		}
		std::string s(p, len);
		p += len;
		return s;
	}

	// ── Template args: I <arg>+ E → "arg1, arg2, ..." ────────────────────────

	std::string parseTemplateArgs(std::vector<std::string>* extractedArgs = nullptr)
	{
		if (!consume('I')) return {};
		std::vector<std::string> args;
		while (!atEnd() && peek() != 'E')
		{
			std::string t = parseType();
			if (t.empty())
			{
				// Non-type or expression argument: skip to E
				// Consume until matching E or end
				int depth = 1;
				if (consume('X'))
				{
					while (!atEnd())
					{
						if (peek() == 'X' || peek() == 'I') ++depth;
						if (peek() == 'E')
						{
							--depth;
							get();
							if (!depth) break;
						}
						else
							get();
					}
					args.push_back("<expr>");
				}
				else if (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '-' || peek() == 'L')
				{
					// literal: skip to end of literal
					if (consume('L'))
					{
						parseType();
						// read integer
						bool neg = consume('n');
						std::string num;
						while (std::isdigit(static_cast<unsigned char>(peek())))
							num += get();
						consume('E');
						args.push_back(neg ? "-" + num : num);
					}
					else
					{
						// Consume the literal, or this arm makes no progress
						// and the enclosing loop spins: `_Z1fI-Ev`, eight
						// characters, hung here forever. A bare integer
						// literal is an optional '-' or 'n' followed by
						// digits.
						if (peek() == '-' || peek() == 'n') get();
						while (std::isdigit(static_cast<unsigned char>(peek())))
							get();
						args.push_back("<int>");
					}
				}
				else
				{
					break;
				}
			}
			else
			{
				args.push_back(t);
			}
		}
		consume('E');
		if (extractedArgs) *extractedArgs = args;
		if (args.empty()) return "<>";
		std::string result = "<";
		for (std::size_t i = 0; i < args.size(); ++i)
		{
			if (i) result += ", ";
			result += args[i];
		}
		result += '>';
		return result;
	}

	// ── Operator name ─────────────────────────────────────────────────────────

	std::string tryOperator()
	{
		static const struct
		{
			const char* code;
			const char* name;
		} ops[] = {
			{"nw", "operator new"},    {"na", "operator new[]"}, {"dl", "operator delete"}, {"da", "operator delete[]"},
			{"ps", "operator+"},       {"ng", "operator-"},      {"ad", "operator&"},       {"de", "operator*"},
			{"co", "operator~"},       {"pl", "operator+"},      {"mi", "operator-"},       {"ml", "operator*"},
			{"dv", "operator/"},       {"rm", "operator%"},      {"an", "operator&"},       {"or", "operator|"},
			{"eo", "operator^"},       {"aS", "operator="},      {"pL", "operator+="},      {"mI", "operator-="},
			{"mL", "operator*="},      {"dV", "operator/="},     {"rM", "operator%="},      {"aN", "operator&="},
			{"oR", "operator|="},      {"eO", "operator^="},     {"ls", "operator<<"},      {"rs", "operator>>"},
			{"lS", "operator<<="},     {"rS", "operator>>="},    {"eq", "operator=="},      {"ne", "operator!="},
			{"lt", "operator<"},       {"gt", "operator>"},      {"le", "operator<="},      {"ge", "operator>="},
			{"ss", "operator<=>"},     {"nt", "operator!"},      {"aa", "operator&&"},      {"oo", "operator||"},
			{"pp", "operator++"},      {"mm", "operator--"},     {"cm", "operator,"},       {"pm", "operator->*"},
			{"pt", "operator->"},      {"cl", "operator()"},     {"ix", "operator[]"},      {"qu", "operator?"},
			{"cv", "operator (cast)"},
		};
		for (auto& e: ops)
		{
			if (end - p >= 2 && p[0] == e.code[0] && p[1] == e.code[1])
			{
				p += 2;
				return e.name;
			}
		}
		return {};
	}

	// ── Ctor/Dtor name ────────────────────────────────────────────────────────

	std::string tryCtorDtor(bool& isCtor, bool& isDtor)
	{
		if (p + 1 < end && p[0] == 'C' && (p[1] >= '1' && p[1] <= '5'))
		{
			isCtor = true;
			p += 2;
			return "<constructor>";
		}
		if (p + 1 < end && p[0] == 'D'
			&& (p[1] == '0' || p[1] == '1' || p[1] == '2' || p[1] == '5' || p[1] == '4' || p[1] == '9'))
		{
			isDtor = true;
			p += 2;
			return "<destructor>";
		}
		return {};
	}

	// ── Substitution ──────────────────────────────────────────────────────────

	std::string trySubstitution()
	{
		if (!consume('S')) return {};
		// S_ = subs[0], S0_ = subs[1], S1_ = subs[2], ...
		// Well-known substitutions
		static const struct
		{
			char c;
			const char* name;
		} well[] = {
			{'t', "std"},
			{'a', "std::allocator"},
			{'b', "std::basic_string"},
			{'s', "std::string"},
			{'i', "std::istream"},
			{'o', "std::ostream"},
			{'d', "std::iostream"},
		};
		for (auto& e: well)
		{
			if (peek() == e.c)
			{
				++p;
				return e.name;
			}
		}
		// Numbered substitutions
		if (peek() == '_')
		{
			++p;
			return subs.empty() ? "<sub0>" : subs[0];
		}

		// <seq-id> is base 36 over 0-9 then A-Z (Itanium ABI 5.1.4). Lower
		// case is not a seq-id digit: St, Sa, Sb, Ss, Si, So and Sd are the
		// well-known substitutions, handled above, and anything else lower
		// case here is malformed. This used to consume it anyway, as a digit
		// worth 36 to 61 -- values base 36 does not have.
		//
		// The accumulation saturates into a std::size_t. It was `idx*36 +
		// digit` in an int, which wrapped negative, and `realIdx < (int)
		// subs.size()` is true for a negative number -- so it read
		// subs[negative]. An ordinary -O1 build with no sanitizer segfaults
		// on _ZN1S1fESZZZZZZZZZZ_E.
		constexpr std::size_t kMaxSeqId = 1u << 20;
		std::size_t idx = 0;
		bool tooLarge = false;
		while (peek() && peek() != '_')
		{
			const char c = peek();
			std::size_t digit;
			if (c >= '0' && c <= '9')
				digit = static_cast<std::size_t>(c - '0');
			else if (c >= 'A' && c <= 'Z')
				digit = static_cast<std::size_t>(c - 'A') + 10;
			else
				break;
			++p;
			if (idx > kMaxSeqId)
				tooLarge = true;
			else
				idx = idx * 36 + digit;
		}
		consume('_');
		if (tooLarge) return "<sub?>";
		const std::size_t realIdx = idx + 1; // S0_ is subs[1]
		if (realIdx < subs.size()) return subs[realIdx];
		return "<sub?>";
	}

	// ── Main type parser ──────────────────────────────────────────────────────

	std::string parseType()
	{
		DepthGuard guard(*this);
		if (!guard.entered)
		{
			ok = false;
			return {};
		}

		if (atEnd()) return {};

		// Qualifiers
		std::string qual;
		while (true)
		{
			if (peek() == 'K')
			{
				++p;
				qual = "const " + qual;
			}
			else if (peek() == 'V')
			{
				++p;
				qual = "volatile " + qual;
			}
			else if (peek() == 'r')
			{
				++p;
				qual = "__restrict " + qual;
			}
			else
				break;
		}

		std::string base;

		// Pointer / reference / rvalue-ref
		if (consume('P'))
		{
			std::string inner = parseType();
			base = inner + "*";
		}
		else if (consume('R'))
		{
			std::string inner = parseType();
			base = inner + "&";
		}
		else if (consume('O'))
		{
			std::string inner = parseType();
			base = inner + "&&";
		}
		else if (consume('U'))
		{
			// Vendor extended type (skip name)
			parseSourceName();
			base = parseType();
		}
		else if (consume('A'))
		{
			// Array: A<len>_<type> or A_<type> (unsized)
			std::string dim;
			while (peek() && peek() != '_' && std::isdigit(static_cast<unsigned char>(peek())))
				dim += get();
			consume('_');
			std::string elem = parseType();
			base = elem + "[" + dim + "]";
		}
		else if (consume('F'))
		{
			// Function type: F <return-type> <param-type>* E
			std::string ret = parseType();
			std::vector<std::string> params;
			while (!atEnd() && peek() != 'E')
			{
				std::string pt = parseType();
				if (pt.empty()) break;
				params.push_back(pt);
			}
			consume('E');
			base = ret + "()(";
			for (std::size_t i = 0; i < params.size(); ++i)
			{
				if (i) base += ", ";
				base += params[i];
			}
			base += ")";
		}
		else if (consume('M'))
		{
			// Pointer to member: M <class-type> <member-type>
			std::string cls = parseType();
			std::string mem = parseType();
			base = mem + " " + cls + "::*";
		}
		else if (consume('T'))
		{
			// Template parameter: Ty = param y
			if (peek() == '_')
			{
				++p;
				base = "<T0>";
			}
			else
			{
				std::size_t idx = 0;
				readDecimal(idx); // saturated; this only names the parameter
				consume('_');
				base = "<T" + std::to_string(idx + 1) + ">";
			}
		}
		else if (peek() == 'S')
		{
			base = trySubstitution();
			if (base.empty())
			{
				ok = false;
				return {};
			}
			// May be followed by template args
			if (peek() == 'I')
			{
				std::vector<std::string> targs;
				std::string ta = parseTemplateArgs(&targs);
				base += ta;
			}
		}
		else if (peek() == 'N')
		{
			base = parseNestedName(nullptr, nullptr, nullptr, nullptr);
		}
		else if (peek() == 'Z')
		{
			// Local name type — skip
			++p;
			base = "<local>";
		}
		else if (peek() == 'D')
		{
			std::string bt = tryBuiltin();
			if (!bt.empty())
				base = bt;
			else
			{
				ok = false;
				return {};
			}
		}
		else
		{
			std::string bt = tryBuiltin();
			if (!bt.empty())
			{
				base = bt;
			}
			else if (std::isdigit(static_cast<unsigned char>(peek())))
			{
				// Source name (class/struct type)
				std::string sn = parseSourceName();
				base = sn;
				if (peek() == 'I')
				{
					std::vector<std::string> targs;
					std::string ta = parseTemplateArgs(&targs);
					base += ta;
				}
				subs.push_back(base);
			}
			else
			{
				ok = false;
				return {};
			}
		}

		if (!qual.empty()) base = qual + base;
		return base;
	}

	// ── Nested name: N [CV] [ref] <prefix>* <unqualified-name> E ────────────

	std::string parseNestedName(std::string* outClass, bool* outConst, bool* outIsCtor, bool* outIsDtor)
	{
		if (!consume('N')) return {};

		bool isConst = false, isVolatile = false;
		while (true)
		{
			if (peek() == 'K')
			{
				++p;
				isConst = true;
			}
			else if (peek() == 'V')
			{
				++p;
				isVolatile = true;
			}
			else if (peek() == 'R' && p + 1 < end && p[1] != 'K')
				break;
			else
				break;
		}
		if (outConst) *outConst = isConst;

		std::vector<std::string> parts;

		// Itanium ABI 5.1.4: the substitution candidates of a nested name are
		// its successive PREFIXES, and the entity's own trailing unqualified
		// name is never one of them. Each component used to be registered on
		// its own, so S0_ resolved to "B" where the prefix is "A::B", and S1_
		// resolved to the function's own name -- `_ZN1A1B1gEPS0_S1_` came back
		// with a second parameter of type "g". Committing the PREVIOUS
		// component as each new one arrives means the last is never committed.
		std::string subPrefix;
		auto commitPrevPrefix = [&]() {
			if (parts.empty()) return;
			if (!subPrefix.empty()) subPrefix += "::";
			subPrefix += parts.back();
			subs.push_back(subPrefix);
		};

		while (!atEnd() && peek() != 'E')
		{
			// Substitution
			if (peek() == 'S')
			{
				std::string s = trySubstitution();
				if (s.empty()) break;
				if (peek() == 'I')
				{
					std::string ta = parseTemplateArgs();
					s += ta;
				}
				commitPrevPrefix();
				parts.push_back(s);
				continue;
			}
			// Template instantiation of last part
			if (peek() == 'I')
			{
				if (!parts.empty())
				{
					// The template arguments belong to the component already
					// in `parts`, which has not been committed yet.
					std::string ta = parseTemplateArgs();
					parts.back() += ta;
				}
				else
				{
					parseTemplateArgs(); // discard
				}
				continue;
			}
			// Ctor/Dtor
			bool isCtor = false, isDtor = false;
			std::string cd = tryCtorDtor(isCtor, isDtor);
			if (!cd.empty())
			{
				if (outIsCtor) *outIsCtor = isCtor;
				if (outIsDtor) *outIsDtor = isDtor;
				commitPrevPrefix();
				parts.push_back(cd);
				continue;
			}
			// Operator
			std::string op = tryOperator();
			if (!op.empty())
			{
				commitPrevPrefix();
				parts.push_back(op);
				continue;
			}
			// Source name
			if (std::isdigit(static_cast<unsigned char>(peek())))
			{
				std::string sn = parseSourceName();
				commitPrevPrefix();
				parts.push_back(sn);
				continue;
			}
			break;
		}
		consume('E');

		if (outClass && parts.size() >= 2)
		{
			// Last part is the function name; everything before is the class chain.
			// Well-known substitutions (Ss → "std::string") are not a class scope.
			// A prefix component is a scope whether or not its rendering
			// contains "::". The test used to be on the rendered text, so a
			// template instantiation was dropped -- a libstdc++ container's
			// allocator argument always renders as std::allocator<...> -- and
			// `_ZNSt6vectorIiSaIiEE9push_backERKi` came back with the class
			// "std", asserting that `this` points at a namespace.
			std::string cls;
			for (std::size_t i = 0; i + 1 < parts.size(); ++i)
			{
				if (!cls.empty()) cls += "::";
				cls += parts[i];
			}
			*outClass = cls;
		}

		if (parts.empty()) return {};
		// Nested name: last component is the function; callers store class
		// chain separately via outClass.
		return parts.back();
	}

	// ── Bare function type ────────────────────────────────────────────────────

	// Fills params. If isTemplate=true, the first type is the return type.
	void parseBareFunction(bool isTemplate, std::string* retType, std::vector<ParamInfo>* params)
	{
		if (isTemplate && retType)
		{
			*retType = parseType();
		}
		while (!atEnd())
		{
			std::string t = parseType();
			if (t.empty() || !ok) break;
			if (t == "void" && params->empty()) break; // () → no params
			ParamInfo pi;
			// Strip trailing const/& for the ParamInfo fields
			if (t.size() > 6 && t.substr(0, 6) == "const ")
			{
				pi.isConst = true;
				t = t.substr(6);
			}
			if (!t.empty() && t.back() == '&')
			{
				pi.ref = RefCategory::LValueRef;
				t.pop_back();
				if (!t.empty() && t.back() == '&')
				{
					pi.ref = RefCategory::RValueRef;
					t.pop_back();
				}
			}
			pi.type = t;
			params->push_back(pi);
		}
	}
};

// ─── Itanium seeder implementation ───────────────────────────────────────────

class ItaniumSeeder : public ITypeSeeder {
public:
	const char* name() const noexcept override
	{
		return "Itanium";
	}

	bool accepts(const std::string& s) const noexcept override
	{
		return s.size() > 3 && s[0] == '_' && s[1] == 'Z';
	}

	SignatureInfo extract(const std::string& symbol) const override
	{
		SignatureInfo info;
		info.mangledName = symbol;

		// ── 1. Full demangled name via __cxa_demangle ─────────────────────────
#ifdef RETDEC_USE_CXXABI_DEMANGLE
		{
			int status = 0;
			char* d = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, &status);
			if (status == 0 && d)
			{
				info.demangledName = d;
				free(d);
			}
		}
#endif
		if (info.demangledName.empty())
		{
			// Fallback: use our minimal parser result as demangled name
		}

		// ── 2. Structural parse ────────────────────────────────────────────────
		const char* raw = symbol.c_str();
		std::size_t len = symbol.size();

		// Skip _Z
		ItaniumParser par(raw + 2, len - 2);

		// noreturn? (_ZNr...)  — actually 'r' prefix after _Z for some compilers
		// more commonly it's in the function attributes, skip for now.

		// Encoding
		std::string funcName;
		std::string className;
		bool isConst = false, isCtor = false, isDtor = false;
		bool isTemplate = false;

		if (par.peek() == 'N')
		{
			funcName = par.parseNestedName(&className, &isConst, &isCtor, &isDtor);
			info.isConst = isConst;
			info.isConstructor = isCtor;
			info.isDestructor = isDtor;
		}
		else if (par.peek() == 'L')
		{
			// Local name: _ZL...
			++par.p;
			funcName = par.parseNestedName(&className, &isConst, &isCtor, &isDtor);
		}
		else
		{
			// Unscoped name (free function or std:: shorthand)
			if (par.peek() == 'S')
			{
				funcName = par.trySubstitution();
			}
			else
			{
				// Try operator
				funcName = par.tryOperator();
				if (funcName.empty())
				{
					// Source name
					funcName = par.parseSourceName();
				}
			}
		}

		// After the name, template args may follow (for a template function)
		if (par.peek() == 'I')
		{
			isTemplate = true;
			std::vector<std::string> targs;
			par.parseTemplateArgs(&targs);
			info.templateArgs = targs;
			// Append to function name for display
			if (!targs.empty())
			{
				std::string ta = "<";
				for (std::size_t i = 0; i < targs.size(); ++i)
				{
					if (i) ta += ", ";
					ta += targs[i];
				}
				ta += ">";
				funcName += ta;
			}
		}

		info.functionName = funcName;
		info.className = className;

		// Rebuild namespace / class split.
		//
		// The "::" that separates them has to be at bracket depth zero:
		// rfind() finds the last one anywhere, and a template argument
		// routinely contains one -- "std::vector<int, std::allocator<int> >"
		// split at the "::" inside the allocator and left the class as
		// "allocator<int> >".
		{
			std::size_t pos = std::string::npos;
			int depth = 0;
			for (std::size_t i = 0; i + 1 < className.size(); ++i)
			{
				const char c = className[i];
				if (c == '<')
					++depth;
				else if (c == '>')
					--depth;
				else if (depth == 0 && c == ':' && className[i + 1] == ':')
					pos = i;
			}
			if (pos != std::string::npos)
			{
				info.namespaceName = className.substr(0, pos);
				info.className = className.substr(pos + 2);
			}
		}

		// this pointer for member functions
		if (!info.className.empty() && !info.isConstructor)
		{
			info.hasThis = true;
			std::string thisBase = className.empty() ? info.className : className;
			// Remove template args from this type for cleanliness
			auto lt = thisBase.find('<');
			if (lt != std::string::npos) thisBase = thisBase.substr(0, lt);
			info.thisType = (isConst ? "const " : "") + thisBase + "*";
		}

		// ── 3. Bare function type (parameters + optional return type) ─────────
		if (par.ok)
		{
			par.parseBareFunction(isTemplate, &info.returnType, &info.params);
		}

		// ── 4. If demangled name still empty, build it from parts ─────────────
		if (info.demangledName.empty())
		{
			info.demangledName = funcName;
		}

		// Mark operators
		if (funcName.find("operator") != std::string::npos)
		{
			info.isOperator = true;
		}

		// Itanium always uses platform default CC (no encoding in the mangling)
		// For x86-32 GCC that is cdecl; for x86-64 it is SysVAmd64.
		// We emit Unknown and let the ABI descriptor decide.
		info.callingConvention = MangledCC::Unknown;

		return info;
	}
};

} // namespace

// ─── Public factory ───────────────────────────────────────────────────────────

std::unique_ptr<ITypeSeeder> makeItaniumSeeder()
{
	return std::make_unique<ItaniumSeeder>();
}

} // namespace type_seed
} // namespace retdec
