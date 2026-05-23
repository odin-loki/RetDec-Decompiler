/**
 * @file src/retdec-decompiler/output_lang.cpp
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "output_lang.h"

#include "retdec/llvmir2hll/llvmir2hll.h"
#include "retdec/utils/io/log.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace retdec {
namespace decompiler {

namespace {

std::string lowerCopy(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

} // anonymous namespace

OutputLangId parseOutputLang(const std::string& token)
{
	const std::string t = lowerCopy(token);
	if (t == "c") return OutputLangId::C;
	if (t == "cpp" || t == "c++" || t == "cxx") return OutputLangId::Cpp;
	if (t == "python" || t == "py") return OutputLangId::Python;
	if (t == "csharp" || t == "cs") return OutputLangId::CSharp;
	if (t == "java") return OutputLangId::Java;
	if (t == "wat" || t == "wasm") return OutputLangId::Wat;
	throw std::runtime_error(
			"[--output-lang] unknown language: " + token
			+ " (expected: c|cpp|python|csharp|java|wat)");
}

const char* outputLangCliName(OutputLangId id)
{
	switch (id) {
	case OutputLangId::C:       return "c";
	case OutputLangId::Cpp:     return "cpp";
	case OutputLangId::Python:  return "python";
	case OutputLangId::CSharp:  return "csharp";
	case OutputLangId::Java:    return "java";
	case OutputLangId::Wat:     return "wat";
	default:                    return "";
	}
}

const char* outputLangFileExtension(OutputLangId id)
{
	switch (id) {
	case OutputLangId::C:       return ".c";
	case OutputLangId::Cpp:     return ".cpp";
	case OutputLangId::Python:  return ".py";
	case OutputLangId::CSharp:  return ".cs";
	case OutputLangId::Java:    return ".java";
	case OutputLangId::Wat:     return ".wat";
	default:                    return ".c";
	}
}

const char* nativeTargetHllId(OutputLangId id)
{
	switch (id) {
	case OutputLangId::C:
	case OutputLangId::Cpp:
		return "c";
	default:
		return "c";
	}
}

OutputLangId defaultOutputLangForManaged(ManagedFormat fmt)
{
	if (const char* hint = managedOutputLangHint(fmt)) {
		try {
			return parseOutputLang(hint);
		} catch (...) {
			return OutputLangId::C;
		}
	}
	return OutputLangId::C;
}

bool isOutputLangCompatibleWithManaged(OutputLangId lang, ManagedFormat fmt)
{
	if (fmt == ManagedFormat::Unknown)
		return true;
	const OutputLangId expected = defaultOutputLangForManaged(fmt);
	return lang == expected;
}

void applyNativeOutputLanguage(OutputLangId lang)
{
	const char* hll = nativeTargetHllId(lang);
	llvmir2hll::setTargetHll(hll);

	if (lang != OutputLangId::C && lang != OutputLangId::Cpp) {
		retdec::utils::io::Log::info()
				<< "[output-lang] Native pipeline only supports C/C++ emitters today; "
				<< "using C backend for requested "
				<< outputLangCliName(lang) << "." << std::endl;
	} else if (lang == OutputLangId::Cpp) {
		retdec::utils::io::Log::info()
				<< "[output-lang] C++ output uses the C HLL writer (C++-styled "
				<< ".cpp extension); dedicated C++ writer pending." << std::endl;
	}
}

} // namespace decompiler
} // namespace retdec
