/**
 * @file src/gui/model/code_highlight.cpp
 * @brief Hand-written per-language scanners (no regex: a view re-tokenizes
 *        visible lines every paint, and a 2 MiB decompile is tokenized once on
 *        load).
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/code_highlight.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace retdec::gui::model {

namespace {

bool isIdentStart(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool isIdentChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool isHex(char c)
{
	return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool isDigit(char c)
{
	return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

std::string lower(std::string_view s)
{
	std::string r(s);
	for (char& c: r)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return r;
}

template <std::size_t N>
bool inList(const std::string_view (&list)[N], std::string_view w)
{
	return std::find(std::begin(list), std::end(list), w) != std::end(list);
}

// ── Word lists (the Qt highlighter's rule sets) ─────────────────────────────

constexpr std::string_view kCKeywords[] = {
	"auto",   "break",  "case",   "const",    "continue", "default", "do",      "double",
	"else",   "enum",   "extern", "float",    "for",      "goto",    "if",      "inline",
	"int",    "long",   "register", "return", "short",    "signed",  "sizeof",  "static",
	"struct", "switch", "typedef", "union",   "unsigned", "void",    "volatile", "while"};

constexpr std::string_view kCxxKeywords[] = {
	"auto",     "break",     "case",      "catch",    "class",    "const",     "constexpr", "continue",
	"default",  "delete",    "do",        "double",   "else",     "enum",      "explicit",  "extern",
	"false",    "float",     "for",       "friend",   "goto",     "if",        "inline",    "int",
	"long",     "mutable",   "namespace", "new",      "nullptr",  "operator",  "private",   "protected",
	"public",   "register",  "return",    "short",    "signed",   "sizeof",    "static",    "struct",
	"switch",   "template",  "this",      "throw",    "true",     "try",       "typedef",   "typeid",
	"typename", "union",     "unsigned",  "using",    "virtual",  "void",      "volatile",  "while",
	"override", "final",     "noexcept",  "static_cast", "reinterpret_cast", "const_cast"};

constexpr std::string_view kCTypes[] = {
	"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",  "int16_t", "int32_t", "int64_t",
	"size_t",  "ptrdiff_t", "uintptr_t", "intptr_t", "bool",  "wchar_t", "char"};

constexpr std::string_view kCSharpKeywords[] = {
	"abstract", "as",       "base",      "bool",     "break",    "byte",     "case",     "catch",
	"char",     "checked",  "class",     "const",    "continue", "decimal",  "default",  "delegate",
	"do",       "double",   "else",      "enum",     "event",    "explicit", "extern",   "false",
	"finally",  "fixed",    "float",     "for",      "foreach",  "goto",     "if",       "implicit",
	"in",       "int",      "interface", "internal", "is",       "lock",     "long",     "namespace",
	"new",      "null",     "object",    "operator", "out",      "override", "params",   "private",
	"protected", "public",  "readonly",  "ref",      "return",   "sbyte",    "sealed",   "short",
	"sizeof",   "stackalloc", "static",  "string",   "struct",   "switch",   "this",     "throw",
	"true",     "try",      "typeof",    "uint",     "ulong",    "unchecked", "unsafe",  "ushort",
	"using",    "virtual",  "void",      "volatile", "while",    "var",      "async",    "await",
	"yield",    "get",      "set",       "value",    "partial"};

constexpr std::string_view kJavaKeywords[] = {
	"abstract", "assert",    "boolean",   "break",      "byte",     "case",      "catch",   "char",
	"class",    "const",     "continue",  "default",    "do",       "double",    "else",    "enum",
	"extends",  "final",     "finally",   "float",      "for",      "goto",      "if",      "implements",
	"import",   "instanceof", "int",      "interface",  "long",     "native",    "new",     "null",
	"package",  "private",   "protected", "public",     "return",   "short",     "static",  "strictfp",
	"super",    "switch",    "synchronized", "this",    "throw",    "throws",    "transient", "true",
	"false",    "try",       "void",      "volatile",   "while",    "var",       "sealed",  "permits",
	"record",   "yield"};

constexpr std::string_view kPythonKeywords[] = {
	"False", "None",   "True",    "and",      "as",     "assert", "async", "await", "break",
	"class", "continue", "def",   "del",      "elif",   "else",   "except", "finally", "for",
	"from",  "global", "if",      "import",   "in",     "is",     "lambda", "nonlocal", "not",
	"or",    "pass",   "raise",   "return",   "try",    "while",  "with",  "yield"};

constexpr std::string_view kPythonBuiltins[] = {
	"int",       "str",        "float",    "bytes",    "bool",     "list",        "dict",         "set",
	"tuple",     "type",       "object",   "complex",  "bytearray", "memoryview", "range",        "slice",
	"frozenset", "property",   "classmethod", "staticmethod", "super", "print",   "len",          "zip",
	"map",       "filter",     "enumerate", "sorted",  "reversed", "any",         "all",          "min",
	"max",       "abs",        "round",    "open",     "input",    "iter",        "next",         "repr",
	"isinstance", "issubclass", "hasattr", "getattr",  "setattr",  "delattr",     "vars",         "dir",
	"id",        "hash",       "callable", "chr",      "ord",      "hex",         "bin",          "oct",
	"self"};

constexpr std::string_view kLuaKeywords[] = {
	"and", "break", "do",   "else", "elseif", "end",    "false",  "for",  "function", "goto", "if",
	"in",  "local", "nil",  "not",  "or",     "repeat", "return", "then", "true",     "until", "while"};

constexpr std::string_view kIrKeywords[] = {
	"define",     "declare",   "call",      "ret",          "br",          "load",       "store",     "alloca",
	"getelementptr", "icmp",   "fcmp",      "phi",          "select",      "switch",     "unreachable", "add",
	"sub",        "mul",       "div",       "udiv",         "sdiv",        "urem",       "srem",      "and",
	"or",         "xor",       "shl",       "lshr",         "ashr",        "trunc",      "zext",      "sext",
	"bitcast",    "inttoptr",  "ptrtoint",  "fadd",         "fsub",        "fmul",       "fdiv",      "frem",
	"fptrunc",    "fpext",     "fptoui",    "fptosi",       "uitofp",      "sitofp",     "global",    "constant",
	"private",    "internal",  "external",  "dso_local",    "nsw",         "nuw",        "inbounds",  "align",
	"eq",         "ne",        "ugt",       "uge",          "ult",         "ule",        "sgt",       "sge",
	"slt",        "sle",       "to",        "tail",         "invoke",      "landingpad", "extractvalue", "insertvalue"};

constexpr std::string_view kIrTypes[] = {
	"void", "float", "double", "ptr", "half", "label", "metadata", "x86_fp80", "fp128", "token"};

constexpr std::string_view kAsmSizeWords[] = {
	"ptr", "byte", "word", "dword", "qword", "tword", "xmmword", "ymmword", "zmmword", "short", "near", "far", "offset", "rel"};

constexpr std::string_view kAsmPrefixes[] = {"rep", "repe", "repz", "repne", "repnz", "lock"};

bool isAsmRegister(std::string_view word)
{
	const std::string w = lower(word);
	static constexpr std::string_view named[] = {
		"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip", "eip", "ip",  "cs",  "ds", "es", "fs",
		"gs",  "ss",  "sp",  "bp",  "si",  "di",  "spl", "bpl", "sil", "dil", "lr",  "pc",  "fp", "xzr", "wzr"};
	if (inList(named, w)) return true;
	if (w.size() == 2 && w[0] >= 'a' && w[0] <= 'd' && (w[1] == 'x' || w[1] == 'h' || w[1] == 'l')) return true;
	if (w.size() == 3 && w[0] == 'e' && std::isalpha(static_cast<unsigned char>(w[1])) && std::isalpha(static_cast<unsigned char>(w[2])))
	{
		// eax, ebx, ecx, edx, esi, edi, ebp, esp (the Qt rule was e[a-z]{2})
		static constexpr std::string_view e32[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"};
		return inList(e32, w);
	}
	auto digitsFrom = [&](std::size_t at, std::size_t& end) {
		end = at;
		while (end < w.size() && isDigit(w[end]))
			++end;
		return end > at;
	};
	std::size_t end = 0;
	// r8..r15 with b/w/d suffix, ARM r0..r15, x0..x30, w0..w30, xmm/ymm/zmm N, st / stN
	if (w[0] == 'r' && digitsFrom(1, end))
		return end == w.size() || (end + 1 == w.size() && (w[end] == 'b' || w[end] == 'w' || w[end] == 'd'));
	if ((w[0] == 'x' || w[0] == 'w') && digitsFrom(1, end)) return end == w.size();
	for (std::string_view v: {std::string_view("xmm"), std::string_view("ymm"), std::string_view("zmm")})
		if (w.starts_with(v) && digitsFrom(3, end)) return end == w.size();
	if (w == "st" || (w.size() == 3 && w[0] == 's' && w[1] == 't' && w[2] >= '0' && w[2] <= '7')) return true;
	if (w.size() >= 2 && w[0] == '$') return true; // MIPS $t0, $sp, ...
	return false;
}

bool isIrIntType(std::string_view w)
{
	if (w.size() < 2 || w[0] != 'i') return false;
	return std::all_of(w.begin() + 1, w.end(), isDigit);
}

class Scanner {
public:
	Scanner(CodeLang lang, std::string_view line): lang_(lang), s_(line) {}

	std::vector<TokenSpan> run(int& state)
	{
		std::size_t i = 0;
		const bool block = hasBlockComments(lang_);
		if (state == kStateInBlockComment)
		{
			if (!block)
				state = kStateNormal;
			else
			{
				const std::size_t e = s_.find("*/");
				if (e == std::string_view::npos)
				{
					push(0, s_.size(), TokenKind::Comment);
					return std::move(out_);
				}
				push(0, e + 2, TokenKind::Comment);
				i = e + 2;
				state = kStateNormal;
			}
		}

		// Line-start constructs.
		const std::size_t first = s_.find_first_not_of(" \t");
		if (first != std::string_view::npos && i <= first)
		{
			if ((lang_ == CodeLang::C || lang_ == CodeLang::Cxx) && s_[first] == '#')
			{
				std::size_t e = commentStartAfter(first);
				push(first, e - first, TokenKind::Preprocessor);
				i = e;
			}
			else if (lang_ == CodeLang::Asm)
				i = asmLinePrefix(first);
			else if (lang_ == CodeLang::IR)
				i = irLinePrefix(first);
		}

		while (i < s_.size())
		{
			const char c = s_[i];
			// Line comments.
			if (lineCommentAt(i))
			{
				push(i, s_.size() - i, TokenKind::Comment);
				break;
			}
			// Block comments.
			if (block && c == '/' && i + 1 < s_.size() && s_[i + 1] == '*')
			{
				const std::size_t e = s_.find("*/", i + 2);
				if (e == std::string_view::npos)
				{
					push(i, s_.size() - i, TokenKind::Comment);
					state = kStateInBlockComment;
					break;
				}
				push(i, e + 2 - i, TokenKind::Comment);
				i = e + 2;
				continue;
			}
			// Strings.
			if (c == '"' || (c == '\'' && singleQuoteStrings()) || (lang_ == CodeLang::CSharp && c == '@' && i + 1 < s_.size() && s_[i + 1] == '"'))
			{
				i = string(i);
				continue;
			}
			// IR values / globals.
			if (lang_ == CodeLang::IR && (c == '%' || c == '@') && i + 1 < s_.size())
			{
				std::size_t e = i + 1;
				if (s_[e] == '"')
				{
					e = s_.find('"', e + 1);
					e = e == std::string_view::npos ? s_.size() : e + 1;
				}
				else
					while (e < s_.size() && (isIdentChar(s_[e]) || s_[e] == '.' || s_[e] == '$' || s_[e] == '-'))
						++e;
				if (e > i + 1)
				{
					push(i, e - i, TokenKind::SsaName);
					i = e;
					continue;
				}
			}
			// Numbers.
			if (isDigit(c) || (c == '-' && negativeNumberAllowed() && i + 1 < s_.size() && isDigit(s_[i + 1]) && (i == 0 || !isIdentChar(s_[i - 1]))))
			{
				i = number(i);
				continue;
			}
			// Identifiers.
			if (isIdentStart(c) || (lang_ == CodeLang::Asm && (c == '.' || c == '$') && i + 1 < s_.size() && isIdentStart(s_[i + 1])))
			{
				std::size_t e = i + 1;
				while (e < s_.size() && (isIdentChar(s_[e]) || ((lang_ == CodeLang::Asm || lang_ == CodeLang::IR) && (s_[e] == '.' || s_[e] == '$'))))
					++e;
				const TokenKind k = classifyWord(s_.substr(i, e - i));
				if (k != TokenKind::Text) push(i, e - i, k);
				i = e;
				continue;
			}
			++i;
		}
		return std::move(out_);
	}

private:
	void push(std::size_t start, std::size_t len, TokenKind k)
	{
		if (len == 0) return;
		if (!out_.empty() && out_.back().kind == k && out_.back().start + out_.back().length == start)
		{
			out_.back().length += static_cast<uint32_t>(len);
			return;
		}
		out_.push_back({static_cast<uint32_t>(start), static_cast<uint32_t>(len), k});
	}

	bool singleQuoteStrings() const
	{
		return lang_ != CodeLang::Asm && lang_ != CodeLang::IR && lang_ != CodeLang::Plain;
	}

	bool negativeNumberAllowed() const
	{
		return lang_ == CodeLang::IR;
	}

	bool lineCommentAt(std::size_t i) const
	{
		const char c = s_[i];
		const bool slashSlash = c == '/' && i + 1 < s_.size() && s_[i + 1] == '/';
		switch (lang_)
		{
		case CodeLang::Asm: return c == ';' || slashSlash || c == '#';
		case CodeLang::IR: return c == ';' || slashSlash;
		case CodeLang::C:
		case CodeLang::Cxx:
		case CodeLang::CSharp:
		case CodeLang::Java: return slashSlash;
		case CodeLang::Python: return c == '#';
		case CodeLang::Lua: return c == '-' && i + 1 < s_.size() && s_[i + 1] == '-';
		case CodeLang::Plain: return false;
		}
		return false;
	}

	/// End of a preprocessor directive: the next comment start outside quotes, or EOL.
	std::size_t commentStartAfter(std::size_t from) const
	{
		char quote = 0;
		for (std::size_t i = from; i < s_.size(); ++i)
		{
			const char c = s_[i];
			if (quote)
			{
				if (c == '\\') ++i;
				else if (c == quote) quote = 0;
				continue;
			}
			if (c == '"') quote = c;
			else if (c == '/' && i + 1 < s_.size() && (s_[i + 1] == '/' || s_[i + 1] == '*'))
				return i;
		}
		return s_.size();
	}

	/// Asm listing prefix: "0x401000:" / "401000:" (address), "name:" (label), then
	/// the raw byte column ("55 89 e5") when an address was present.
	std::size_t asmLinePrefix(std::size_t first)
	{
		std::size_t e = first;
		while (e < s_.size() && (isIdentChar(s_[e]) || s_[e] == '.' || s_[e] == '@' || s_[e] == '?' || s_[e] == '$'))
			++e;
		if (e == first || e >= s_.size() || s_[e] != ':') return first;
		const std::string_view word = s_.substr(first, e - first);
		std::string_view digits = word;
		if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) digits.remove_prefix(2);
		const bool isAddr = !digits.empty() && std::all_of(digits.begin(), digits.end(), isHex) && (word.size() >= 4 || word.starts_with("0x"));
		push(first, e + 1 - first, isAddr ? TokenKind::Address : TokenKind::Label);
		std::size_t i = e + 1;
		if (!isAddr) return i;
		// Byte column: whitespace-separated two-hex-digit groups.
		for (;;)
		{
			std::size_t j = i;
			while (j < s_.size() && (s_[j] == ' ' || s_[j] == '\t'))
				++j;
			if (j + 2 <= s_.size() && isHex(s_[j]) && isHex(s_[j + 1]) && (j + 2 == s_.size() || s_[j + 2] == ' ' || s_[j + 2] == '\t'))
			{
				push(j, 2, TokenKind::Number);
				i = j + 2;
				continue;
			}
			return i;
		}
	}

	/// IR line prefix: "%bb1:" / "entry:" / "5:" basic-block headers.
	std::size_t irLinePrefix(std::size_t first)
	{
		std::size_t e = first;
		if (s_[e] == '%') ++e;
		while (e < s_.size() && (isIdentChar(s_[e]) || s_[e] == '.' || s_[e] == '-' || s_[e] == '$'))
			++e;
		if (e == first || e >= s_.size() || s_[e] != ':') return first;
		push(first, e + 1 - first, TokenKind::Label);
		return e + 1;
	}

	std::size_t string(std::size_t i)
	{
		std::size_t j = i;
		if (s_[j] == '@') ++j;
		const char q = s_[j];
		++j;
		while (j < s_.size() && s_[j] != q)
			j += (s_[j] == '\\') ? 2 : 1;
		j = std::min(s_.size(), j + 1);
		push(i, j - i, TokenKind::String);
		return j;
	}

	std::size_t number(std::size_t i)
	{
		std::size_t j = i;
		if (s_[j] == '-') ++j;
		bool hex = false;
		if (s_[j] == '0' && j + 1 < s_.size() && (s_[j + 1] == 'x' || s_[j + 1] == 'X'))
		{
			hex = true;
			j += 2;
			while (j < s_.size() && isHex(s_[j]))
				++j;
		}
		else
		{
			while (j < s_.size() && (isDigit(s_[j]) || s_[j] == '.'))
				++j;
			// e+10 exponents
			if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E') && j + 1 < s_.size() && (isDigit(s_[j + 1]) || s_[j + 1] == '+' || s_[j + 1] == '-'))
			{
				j += 2;
				while (j < s_.size() && isDigit(s_[j]))
					++j;
			}
		}
		// Literal suffixes (u, l, f, d, m, j, h for asm hex) -- but a number running
		// into identifier characters is part of a word (e.g. "1st"), not a literal.
		std::size_t k = j;
		while (k < s_.size() && std::string_view("uUlLfFdDmMjJ").find(s_[k]) != std::string_view::npos)
			++k;
		if (lang_ == CodeLang::Asm && k == j && k < s_.size() && (s_[k] == 'h' || s_[k] == 'H')) ++k;
		if (k < s_.size() && isIdentChar(s_[k]))
		{
			while (k < s_.size() && isIdentChar(s_[k]))
				++k;
			return k; // e.g. "3dnow": plain text
		}
		push(i, k - i, (hex && lang_ == CodeLang::Asm) ? TokenKind::Address : TokenKind::Number);
		return k;
	}

	TokenKind classifyWord(std::string_view w)
	{
		switch (lang_)
		{
		case CodeLang::Plain: return TokenKind::Text;
		case CodeLang::Asm: {
			const std::string lw = lower(w);
			if (!sawMnemonic_ && !(lw.size() >= 2 && lw[0] == '.'))
			{
				if (!inList(kAsmPrefixes, lw)) sawMnemonic_ = true;
				return TokenKind::Mnemonic;
			}
			if (lw.size() >= 2 && lw[0] == '.' && !sawMnemonic_) return TokenKind::Keyword; // directive
			if (isAsmRegister(lw)) return TokenKind::Register;
			if (inList(kAsmSizeWords, lw)) return TokenKind::Keyword;
			return TokenKind::Text;
		}
		case CodeLang::IR:
			if (isIrIntType(w) || inList(kIrTypes, w)) return TokenKind::Type;
			if (inList(kIrKeywords, w)) return TokenKind::Keyword;
			return TokenKind::Text;
		case CodeLang::C:
			if (inList(kCKeywords, w)) return TokenKind::Keyword;
			if (inList(kCTypes, w)) return TokenKind::Type;
			return TokenKind::Text;
		case CodeLang::Cxx:
			if (inList(kCxxKeywords, w)) return TokenKind::Keyword;
			if (inList(kCTypes, w)) return TokenKind::Type;
			return TokenKind::Text;
		case CodeLang::CSharp: return inList(kCSharpKeywords, w) ? TokenKind::Keyword : TokenKind::Text;
		case CodeLang::Java: return inList(kJavaKeywords, w) ? TokenKind::Keyword : TokenKind::Text;
		case CodeLang::Python:
			if (inList(kPythonKeywords, w)) return TokenKind::Keyword;
			if (inList(kPythonBuiltins, w)) return TokenKind::Builtin;
			return TokenKind::Text;
		case CodeLang::Lua: return inList(kLuaKeywords, w) ? TokenKind::Keyword : TokenKind::Text;
		}
		return TokenKind::Text;
	}

	CodeLang lang_;
	std::string_view s_;
	std::vector<TokenSpan> out_;
	bool sawMnemonic_ = false;
};

} // namespace

bool hasBlockComments(CodeLang lang)
{
	return lang == CodeLang::C || lang == CodeLang::Cxx || lang == CodeLang::CSharp || lang == CodeLang::Java;
}

std::vector<TokenSpan> highlightLine(CodeLang lang, std::string_view line, int& state)
{
	if (lang == CodeLang::Plain)
	{
		state = kStateNormal;
		return {};
	}
	Scanner sc(lang, line);
	return sc.run(state);
}

std::vector<std::vector<TokenSpan>> highlightLines(CodeLang lang, const std::vector<std::string>& lines)
{
	std::vector<std::vector<TokenSpan>> out;
	out.reserve(lines.size());
	int state = kStateNormal;
	for (const std::string& l: lines)
		out.push_back(highlightLine(lang, l, state));
	return out;
}

TokenKind tokenAt(const std::vector<TokenSpan>& spans, uint32_t column)
{
	for (const TokenSpan& s: spans)
	{
		if (column < s.start) break;
		if (column < s.start + s.length) return s.kind;
	}
	return TokenKind::Text;
}

CodeLang codeLangFromSettingsString(std::string_view s)
{
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
		s.remove_prefix(1);
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
		s.remove_suffix(1);
	const std::string k = lower(s);
	if (k == "cpp" || k == "c++") return CodeLang::Cxx;
	if (k == "csharp" || k == "cs" || k == "c#") return CodeLang::CSharp;
	if (k == "java") return CodeLang::Java;
	if (k == "python" || k == "py") return CodeLang::Python;
	if (k == "lua") return CodeLang::Lua;
	return CodeLang::C;
}

std::string_view codeLangName(CodeLang lang)
{
	switch (lang)
	{
	case CodeLang::Plain: return "Text";
	case CodeLang::Asm: return "Assembly";
	case CodeLang::IR: return "IR";
	case CodeLang::C: return "C";
	case CodeLang::Cxx: return "C++";
	case CodeLang::CSharp: return "C#";
	case CodeLang::Java: return "Java";
	case CodeLang::Python: return "Python";
	case CodeLang::Lua: return "Lua";
	}
	return "C";
}

} // namespace retdec::gui::model
