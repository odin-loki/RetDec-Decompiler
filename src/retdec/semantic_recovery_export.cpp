/**
 * @file src/retdec/semantic_recovery_export.cpp
 * @brief Export post-pipeline semantic detections to config JSON and decompiled C.
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/retdec/semantic_recovery_export.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <vector>

namespace retdec {
namespace analysis {

namespace {

common::SemanticDetection
makeDetection(const std::string& kind, const std::string& label, float confidence, const std::string& detail = {})
{
	common::SemanticDetection d;
	d.kind = kind;
	d.label = label;
	d.confidence = confidence;
	d.detail = detail;
	return d;
}

void appendDetection(SemanticDetectionMap& map, const std::string& fnName, common::SemanticDetection detection)
{
	if (fnName.empty())
	{
		return;
	}
	map[fnName].push_back(std::move(detection));
}

void collectConcurrencyDetections(const concurrency_detect::ConcurrencyModel& cm, SemanticDetectionMap& map)
{
	auto addFn = [&](const std::string& fnName, const std::string& label, float confidence, const std::string& extra) {
		std::string detail = "evidence:symbol_name";
		if (!extra.empty())
		{
			detail += " ";
			detail += extra;
		}
		appendDetection(map, fnName, makeDetection("concurrency", label, confidence, detail));
	};

	for (const auto& t: cm.threads)
	{
		addFn(t.funcName, "thread", 0.75f, t.threadFunc);
	}
	for (const auto& l: cm.locks)
	{
		addFn(l.funcName, "mutex", 0.75f, "");
	}
	for (const auto& a: cm.atomics)
	{
		addFn(a.funcName, "atomic", 0.70f, a.varName);
	}
	for (const auto& c: cm.condVars)
	{
		addFn(c.funcName, "condition_variable", 0.70f, "");
	}
	for (const auto& s: cm.spinlocks)
	{
		addFn(s.funcName, "spinlock", 0.65f, "");
	}
}

void injectSemanticCommentsIntoLines(std::vector<std::string>& lines, const config::Config& config)
{
	const std::string& outputLang = config.parameters.getOutputLang();
	std::map<int, std::vector<std::string>> inserts;
	for (const auto& fn: config.functions)
	{
		if (fn.semanticDetections.empty() || !fn.getStartLine().isDefined())
		{
			continue;
		}

		const int line = static_cast<int>(fn.getStartLine().getValue());
		if (line <= 0)
		{
			continue;
		}

		for (const auto& d: fn.semanticDetections)
		{
			const std::string comment = "// " + d.commentLine(outputLang);
			bool duplicate = false;
			if (line > 1 && line - 2 < static_cast<int>(lines.size()))
			{
				const auto& prev = lines[static_cast<std::size_t>(line - 2)];
				if (prev.find(comment) != std::string::npos)
				{
					duplicate = true;
				}
			}
			if (!duplicate)
			{
				inserts[line].push_back(comment);
			}
		}
	}

	if (inserts.empty())
	{
		return;
	}

	for (auto it = inserts.rbegin(); it != inserts.rend(); ++it)
	{
		const int idx = it->first - 1;
		if (idx < 0 || idx > static_cast<int>(lines.size()))
		{
			continue;
		}
		for (auto cit = it->second.rbegin(); cit != it->second.rend(); ++cit)
		{
			lines.insert(lines.begin() + idx, *cit);
		}
	}
}

bool emitBuildableEnabled()
{
	const char* e = std::getenv("RETDEC_EMIT_BUILDABLE");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

bool isIdentStart(char c)
{
	return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentCont(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

void skipSpaces(const std::string& s, std::size_t& i);
void skipSpacesBack(const std::string& s, std::size_t& i);
std::size_t skipNonCode(const std::string& s, std::size_t i);
std::size_t matchingCloseParen(const std::string& s, std::size_t open);
bool bodyUsesIdent(const std::string& body, const std::string& name);
bool looksLikeDeclarator(const std::string& s, std::size_t namePos);
bool isDefinitionAfterClose(const std::string& s, std::size_t close);
std::string appendMissingGotoLabels(const std::string& body);
bool identUsedAsPointer(const std::string& body, const std::string& name);
bool isPthreadName(const std::string& name);
bool isAsmIntrinsicName(const std::string& name);

std::string outputStem(const std::string& outputCPath)
{
	const auto slash = outputCPath.find_last_of("/\\");
	const auto dot = outputCPath.rfind('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
	{
		return outputCPath.substr(0, dot);
	}
	return outputCPath;
}

std::string pathBasename(const std::string& path)
{
	const auto slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string includeGuardFromHeaderName(const std::string& headerName)
{
	std::string g;
	g.reserve(headerName.size() + 2);
	for (char c: headerName)
	{
		if (std::isalnum(static_cast<unsigned char>(c)) != 0)
		{
			g += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		}
		else
		{
			g += '_';
		}
	}
	if (g.empty() || std::isdigit(static_cast<unsigned char>(g[0])) != 0)
	{
		g = "RETDEC_" + g;
	}
	return g;
}

bool isSkippedCallName(const std::string& name)
{
	return name == "if" || name == "for" || name == "while" || name == "switch" || name == "return" || name == "sizeof"
		|| name == "__readUndefQword" || name == "__readfsqword" || name == "__asm_hlt"
		|| name == "__asm_rep_stosq_memset" || name == "__retdec_stub";
}

const char* libcHeaderForName(const std::string& name)
{
	if (name == "memcpy" || name == "memmove" || name == "memset" || name == "memcmp" || name == "memchr"
		|| name == "strcpy" || name == "strncpy" || name == "strcat" || name == "strncat" || name == "strcmp"
		|| name == "strncmp" || name == "strlen" || name == "strchr" || name == "strrchr" || name == "strstr"
		|| name == "strtok" || name == "strdup" || name == "strerror")
	{
		return "string.h";
	}
	if (name == "printf" || name == "fprintf" || name == "sprintf" || name == "snprintf" || name == "vprintf"
		|| name == "vfprintf" || name == "vsprintf" || name == "vsnprintf" || name == "puts" || name == "putchar"
		|| name == "putc" || name == "getchar" || name == "getc" || name == "scanf" || name == "fscanf"
		|| name == "sscanf" || name == "fopen" || name == "fclose" || name == "fread" || name == "fwrite"
		|| name == "fgets" || name == "fputs" || name == "fflush" || name == "fseek" || name == "ftell"
		|| name == "rewind" || name == "feof" || name == "perror")
	{
		return "stdio.h";
	}
	if (name == "malloc" || name == "calloc" || name == "realloc" || name == "free" || name == "exit" || name == "abort"
		|| name == "atexit" || name == "atoi" || name == "atol" || name == "atoll" || name == "atof" || name == "abs"
		|| name == "labs" || name == "llabs" || name == "qsort" || name == "bsearch")
	{
		return "stdlib.h";
	}
	if (name == "isalpha" || name == "isdigit" || name == "isspace" || name == "tolower" || name == "toupper")
	{
		return "ctype.h";
	}
	return nullptr;
}

bool isLibcCallName(const std::string& name)
{
	return libcHeaderForName(name) != nullptr;
}

bool isCrtCallName(const std::string& name)
{
	return name == "__cxa_finalize" || name == "__cxa_atexit" || name == "__gmon_start__" || name == "__stack_chk_fail"
		|| name == "__libc_start_main" || name == "__do_global_dtors_aux" || name == "deregister_tm_clones"
		|| name == "register_tm_clones" || name == "frame_dummy" || name == "_init" || name == "_fini"
		|| name == "__printf_chk" || name == "__fprintf_chk" || name == "__sprintf_chk" || name == "__snprintf_chk"
		|| name == "__memcpy_chk" || name == "__memmove_chk" || name == "__memset_chk" || name == "__strcpy_chk"
		|| name == "__strncpy_chk";
}

bool isPthreadName(const std::string& name)
{
	return name.compare(0, 8, "pthread_") == 0;
}

bool isAsmIntrinsicName(const std::string& name)
{
	return name.compare(0, 6, "__asm_") == 0 && name != "__asm_hlt" && name != "__asm_rep_stosq_memset";
}

bool sourceDefinesMain(const std::string& src)
{
	return src.find("int main(") != std::string::npos || src.find("void main(") != std::string::npos
		|| src.find("uint64_t main(") != std::string::npos || src.find("int64_t main(") != std::string::npos
		|| src.find("int32_t main(") != std::string::npos || src.find("int16_t main(") != std::string::npos;
}

void writeWeakMacro(std::ostream& os)
{
	os << "#if defined(__GNUC__)\n";
	os << "#define RETDEC_BUILDABLE_WEAK __attribute__((weak))\n";
	os << "#else\n";
	os << "#define RETDEC_BUILDABLE_WEAK\n";
	os << "#endif\n";
}

void writeHelperStubs(std::ostream& os)
{
	writeWeakMacro(os);
	os << "RETDEC_BUILDABLE_WEAK uint64_t __readUndefQword(void) { return 0; }\n";
	os << "RETDEC_BUILDABLE_WEAK uint64_t __readfsqword(unsigned long a) "
		  "{ (void)a; return 0; }\n";
	os << "RETDEC_BUILDABLE_WEAK void __asm_hlt(void) {}\n";
	os << "RETDEC_BUILDABLE_WEAK void *__asm_rep_stosq_memset(void *d, int c, "
		  "unsigned long n) { return memset(d, c, (size_t)n); }\n";
}

void writeLibcArityMacros(std::ostream& os)
{
	os << "#define strncpy(dst, src, n, ...) "
		  "(strncpy)((char *)(dst), (const char *)(src), (size_t)(n))\n";
	os << "#define strcmp(a, b, ...) (strcmp)((a), (b))\n";
	os << "#define strncmp(a, b, n, ...) (strncmp)((a), (b), (n))\n";
	os << "#define puts(s, ...) (puts)((s))\n";
	os << "#define putc(ch, ...) (putc)((int)(ch), stdout)\n";
	os << "#define putchar(ch, ...) (putchar)((int)(ch))\n";
	os << "static inline uint64_t retdec_pthread_any(void *a, ...) { (void)a; return 0; }\n";
	os << "#define pthread_create(...) retdec_pthread_any(__VA_ARGS__)\n";
	os << "#define pthread_join(...) retdec_pthread_any(__VA_ARGS__)\n";
	os << "#define pthread_mutex_lock(...) retdec_pthread_any(__VA_ARGS__)\n";
	os << "#define pthread_mutex_unlock(...) retdec_pthread_any(__VA_ARGS__)\n";
	os << "#define pthread_mutex_init(...) retdec_pthread_any(__VA_ARGS__)\n";
	os << "#define pthread_mutex_destroy(...) retdec_pthread_any(__VA_ARGS__)\n";
}

void writePrototypeBodies(std::ostream& os, const std::string& src)
{
	std::set<std::string> defined;
	std::vector<std::pair<std::string, std::string>> protos;
	std::size_t i = 0;
	while (i < src.size())
	{
		const std::size_t next = skipNonCode(src, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(src[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < src.size() && isIdentCont(src[i]))
		{
			++i;
		}
		std::size_t after = i;
		skipSpaces(src, after);
		if (after >= src.size() || src[after] != '(')
		{
			continue;
		}
		const std::string name = src.substr(start, i - start);
		if (isSkippedCallName(name) || isLibcCallName(name) || isPthreadName(name) || isAsmIntrinsicName(name))
		{
			i = after + 1;
			continue;
		}
		const std::size_t close = matchingCloseParen(src, after);
		if (close == std::string::npos)
		{
			i = after + 1;
			continue;
		}
		if (!looksLikeDeclarator(src, start))
		{
			i = after + 1;
			continue;
		}
		if (isDefinitionAfterClose(src, close))
		{
			defined.insert(name);
			i = after + 1;
			continue;
		}
		std::size_t semi = close + 1;
		skipSpaces(src, semi);
		if (semi >= src.size() || src[semi] != ';')
		{
			i = after + 1;
			continue;
		}
		std::size_t lineStart = start;
		while (lineStart > 0 && src[lineStart - 1] != '\n')
		{
			--lineStart;
		}
		if (lineStart < start && (src[lineStart] == ' ' || src[lineStart] == '\t'))
		{
			i = after + 1;
			continue;
		}
		std::size_t begin = lineStart;
		skipSpaces(src, begin);
		if (begin >= start)
		{
			i = after + 1;
			continue;
		}
		const std::string proto = src.substr(begin, close + 1 - begin);
		protos.emplace_back(name, proto);
		i = after + 1;
	}
	for (const auto& item: protos)
	{
		if (defined.count(item.first) != 0)
		{
			continue;
		}
		os << "RETDEC_BUILDABLE_WEAK " << item.second;
		if (item.second.compare(0, 5, "void ") == 0 && (item.second.size() < 6 || item.second[5] != '*'))
		{
			os << " {}\n";
		}
		else
		{
			os << " { return 0; }\n";
		}
	}
}

bool isControlName(const std::string& name)
{
	return name == "if" || name == "for" || name == "while" || name == "switch" || name == "else";
}

bool isTypeName(const std::string& name)
{
	if (name == "int" || name == "long" || name == "short" || name == "char" || name == "void" || name == "unsigned"
		|| name == "signed" || name == "bool" || name == "float" || name == "double" || name == "struct"
		|| name == "enum" || name == "union" || name == "volatile")
	{
		return true;
	}
	return name.size() >= 3 && name.compare(name.size() - 2, 2, "_t") == 0;
}

bool isForInitDecl(const std::string& body, std::size_t namePos)
{
	int depth = 0;
	for (std::size_t k = namePos; k > 0; --k)
	{
		const char c = body[k - 1];
		if (c == ')')
		{
			++depth;
		}
		else if (c == '(')
		{
			if (depth == 0)
			{
				std::size_t t = k - 1;
				skipSpacesBack(body, t);
				return t >= 3 && body.compare(t - 3, 3, "for") == 0 && (t == 3 || !isIdentCont(body[t - 4]));
			}
			--depth;
		}
		else if (c == '{' || c == '}' || c == ';')
		{
			if (depth == 0)
			{
				return false;
			}
		}
	}
	return false;
}

bool bodyDeclaresIdent(const std::string& body, const std::string& name)
{
	std::size_t i = 0;
	while (i < body.size())
	{
		const std::size_t next = skipNonCode(body, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(body[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < body.size() && isIdentCont(body[i]))
		{
			++i;
		}
		if (body.compare(start, i - start, name) != 0)
		{
			continue;
		}
		std::size_t back = start;
		while (back > 0 && std::isspace(static_cast<unsigned char>(body[back - 1])) != 0)
		{
			--back;
		}
		if (back > 0 && body[back - 1] == '*')
		{
			std::size_t t = back;
			while (t > 0 && (body[t - 1] == '*' || std::isspace(static_cast<unsigned char>(body[t - 1])) != 0))
			{
				--t;
			}
			if (t > 0 && isIdentCont(body[t - 1]))
			{
				std::size_t ts = t;
				while (ts > 0 && isIdentCont(body[ts - 1]))
				{
					--ts;
				}
				const std::string ty = body.substr(ts, t - ts);
				if ((isTypeName(ty) || ty == "unsigned" || ty == "const") && !isForInitDecl(body, start))
				{
					return true;
				}
			}
			continue;
		}
		if (back > 0 && isIdentCont(body[back - 1]))
		{
			std::size_t t = back;
			while (t > 0 && isIdentCont(body[t - 1]))
			{
				--t;
			}
			const std::string ty = body.substr(t, back - t);
			if ((isTypeName(ty) || ty == "unsigned" || ty == "const") && !isForInitDecl(body, start))
			{
				return true;
			}
		}
	}
	return false;
}

bool identUsedAsPointer(const std::string& body, const std::string& name)
{
	std::size_t i = 0;
	while (i < body.size())
	{
		const std::size_t next = skipNonCode(body, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(body[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < body.size() && isIdentCont(body[i]))
		{
			++i;
		}
		if (body.compare(start, i - start, name) != 0)
		{
			continue;
		}
		std::size_t back = start;
		while (back > 0 && std::isspace(static_cast<unsigned char>(body[back - 1])) != 0)
		{
			--back;
		}
		if (back > 0 && body[back - 1] == '*')
		{
			std::size_t t = back;
			while (t > 0 && (body[t - 1] == '*' || std::isspace(static_cast<unsigned char>(body[t - 1])) != 0))
			{
				--t;
			}
			if (t == 0 || !isIdentCont(body[t - 1]))
			{
				return true;
			}
			std::size_t ts = t;
			while (ts > 0 && isIdentCont(body[ts - 1]))
			{
				--ts;
			}
			const std::string ty = body.substr(ts, t - ts);
			if (!isTypeName(ty) && ty != "unsigned" && ty != "const")
			{
				return true;
			}
		}
	}
	return false;
}

bool bodyUsesIdent(const std::string& body, const std::string& name)
{
	std::size_t i = 0;
	while (i < body.size())
	{
		const std::size_t next = skipNonCode(body, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(body[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < body.size() && isIdentCont(body[i]))
		{
			++i;
		}
		if (body.compare(start, i - start, name) == 0)
		{
			return true;
		}
	}
	return false;
}

std::vector<std::string> missingTempsInBody(const std::string& body)
{
	std::vector<std::string> missing;
	std::set<std::string> names = {"result", "result2", "result3", "thread", "status", "c", "str", "str2"};
	std::size_t i = 0;
	while (i < body.size())
	{
		const std::size_t next = skipNonCode(body, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(body[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < body.size() && isIdentCont(body[i]))
		{
			++i;
		}
		const std::string name = body.substr(start, i - start);
		if (name.size() >= 2 && name[0] == 'v')
		{
			bool digits = true;
			for (std::size_t k = 1; k < name.size(); ++k)
			{
				if (std::isdigit(static_cast<unsigned char>(name[k])) == 0)
				{
					digits = false;
					break;
				}
			}
			if (digits)
			{
				names.insert(name);
			}
		}
	}
	for (const auto& name: names)
	{
		if (bodyUsesIdent(body, name) && !bodyDeclaresIdent(body, name))
		{
			missing.push_back(name);
		}
	}
	return missing;
}

// Sidecar-only: RetDec can emit `goto lab_0x...` with no matching label.
std::string appendMissingGotoLabels(const std::string& body)
{
	std::set<std::string> gotos;
	std::set<std::string> labels;
	std::size_t i = 0;
	while (i < body.size())
	{
		const std::size_t next = skipNonCode(body, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(body[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < body.size() && isIdentCont(body[i]))
		{
			++i;
		}
		const std::string name = body.substr(start, i - start);
		if (name == "goto")
		{
			std::size_t after = i;
			skipSpaces(body, after);
			if (after < body.size() && isIdentStart(body[after]))
			{
				const std::size_t ls = after;
				++after;
				while (after < body.size() && isIdentCont(body[after]))
				{
					++after;
				}
				gotos.insert(body.substr(ls, after - ls));
				i = after;
			}
			continue;
		}
		if (name == "default" || name == "case")
		{
			continue;
		}
		std::size_t after = i;
		skipSpaces(body, after);
		if (after < body.size() && body[after] == ':')
		{
			labels.insert(name);
		}
	}
	std::string extra;
	for (const auto& lab: gotos)
	{
		if (labels.find(lab) == labels.end())
		{
			extra += "    ";
			extra += lab;
			extra += ": ;\n";
		}
	}
	if (extra.empty())
	{
		return body;
	}
	return body + extra;
}

std::string stripSidecarSystemIncludes(const std::string& src)
{
	std::string out;
	out.reserve(src.size());
	std::istringstream iss(src);
	std::string line;
	while (std::getline(iss, line))
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		std::size_t p = 0;
		while (p < line.size() && (line[p] == ' ' || line[p] == '\t'))
		{
			++p;
		}
		if (p < line.size() && line.compare(p, 10, "#include <") == 0)
		{
			if (line.find("<pthread.h>") != std::string::npos || line.find("<stdio.h>") != std::string::npos
				|| line.find("<string.h>") != std::string::npos || line.find("<stdlib.h>") != std::string::npos)
			{
				continue;
			}
		}
		out += line;
		out += '\n';
	}
	return out;
}

void collectAsmIntrinsicNames(const std::string& src, std::set<std::string>& names)
{
	std::size_t i = 0;
	while (i < src.size())
	{
		const std::size_t next = skipNonCode(src, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(src[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < src.size() && isIdentCont(src[i]))
		{
			++i;
		}
		const std::string name = src.substr(start, i - start);
		if (isAsmIntrinsicName(name))
		{
			names.insert(name);
		}
	}
}

std::size_t matchingCloseBrace(const std::string& s, std::size_t open)
{
	if (open >= s.size() || s[open] != '{')
	{
		return std::string::npos;
	}
	int depth = 0;
	for (std::size_t i = open; i < s.size();)
	{
		const std::size_t next = skipNonCode(s, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (s[i] == '{')
		{
			++depth;
		}
		else if (s[i] == '}')
		{
			--depth;
			if (depth == 0)
			{
				return i;
			}
		}
		++i;
	}
	return std::string::npos;
}

std::string injectUndeclaredTemps(const std::string& src)
{
	std::string out;
	out.reserve(src.size() + 256);
	std::size_t i = 0;
	while (i < src.size())
	{
		const std::size_t next = skipNonCode(src, i);
		if (next != i)
		{
			out.append(src, i, next - i);
			i = next;
			continue;
		}
		if (!isIdentStart(src[i]))
		{
			out.push_back(src[i]);
			++i;
			continue;
		}
		const std::size_t nameStart = i;
		++i;
		while (i < src.size() && isIdentCont(src[i]))
		{
			++i;
		}
		const std::string name = src.substr(nameStart, i - nameStart);
		out.append(src, nameStart, i - nameStart);
		std::size_t after = i;
		skipSpaces(src, after);
		if (after >= src.size() || src[after] != '(' || isControlName(name) || isTypeName(name))
		{
			continue;
		}
		const std::size_t close = matchingCloseParen(src, after);
		if (close == std::string::npos)
		{
			continue;
		}
		out.append(src, i, close + 1 - i);
		i = close + 1;
		std::size_t brace = i;
		skipSpaces(src, brace);
		if (brace >= src.size() || src[brace] != '{')
		{
			continue;
		}
		out.append(src, i, brace + 1 - i);
		const std::size_t bodyEnd = matchingCloseBrace(src, brace);
		if (bodyEnd == std::string::npos)
		{
			i = brace + 1;
			continue;
		}
		const std::string params = src.substr(after + 1, close - after - 1);
		std::set<std::string> skip;
		skip.insert(name);
		{
			std::size_t p = 0;
			while (p < params.size())
			{
				if (!isIdentStart(params[p]))
				{
					++p;
					continue;
				}
				const std::size_t ps = p;
				++p;
				while (p < params.size() && isIdentCont(params[p]))
				{
					++p;
				}
				skip.insert(params.substr(ps, p - ps));
			}
		}
		std::string body = src.substr(brace + 1, bodyEnd - brace - 1);
		body = appendMissingGotoLabels(body);
		auto missing = missingTempsInBody(body);
		missing.erase(
			std::remove_if(missing.begin(), missing.end(), [&](const std::string& id) { return skip.count(id) != 0; }),
			missing.end());
		if (!missing.empty())
		{
			out += '\n';
			for (const auto& ident: missing)
			{
				if (identUsedAsPointer(body, ident))
				{
					out += "    int64_t * ";
				}
				else
				{
					out += "    int64_t ";
				}
				out += ident;
				out += " = 0;\n";
			}
		}
		out += body;
		out.push_back('}');
		i = bodyEnd + 1;
	}
	return out;
}

// Sidecar-only: RetDec can emit `break`/`continue` outside a loop/switch
// (hash_table function_1305). Do not change the default .c.
std::string rewriteOrphanBreakContinue(const std::string& src)
{
	std::string out;
	out.reserve(src.size() + 64);
	enum class BraceKind
	{
		Other,
		Loop,
		Switch
	};
	std::vector<BraceKind> stack;
	BraceKind pending = BraceKind::Other;
	int paren = 0;
	std::size_t i = 0;
	while (i < src.size())
	{
		const std::size_t next = skipNonCode(src, i);
		if (next != i)
		{
			out.append(src, i, next - i);
			i = next;
			continue;
		}
		if (isIdentStart(src[i]))
		{
			const std::size_t start = i;
			++i;
			while (i < src.size() && isIdentCont(src[i]))
			{
				++i;
			}
			const std::string name = src.substr(start, i - start);
			if (name == "for" || name == "while" || name == "do")
			{
				out.append(src, start, i - start);
				pending = BraceKind::Loop;
				continue;
			}
			if (name == "switch")
			{
				out.append(src, start, i - start);
				pending = BraceKind::Switch;
				continue;
			}
			if (name == "break" || name == "continue")
			{
				std::size_t after = i;
				skipSpaces(src, after);
				if (after < src.size() && src[after] == ';')
				{
					bool inLoop = pending == BraceKind::Loop;
					bool inSwitch = pending == BraceKind::Switch;
					for (auto k: stack)
					{
						if (k == BraceKind::Loop)
						{
							inLoop = true;
						}
						if (k == BraceKind::Switch)
						{
							inSwitch = true;
						}
					}
					const bool ok = (name == "break") ? (inLoop || inSwitch) : inLoop;
					if (!ok)
					{
						out += "/* orphan ";
						out += name;
						out += " */";
						continue;
					}
				}
			}
			out.append(src, start, i - start);
			continue;
		}
		if (src[i] == '{')
		{
			stack.push_back(pending);
			pending = BraceKind::Other;
			out.push_back('{');
			++i;
			continue;
		}
		if (src[i] == '}')
		{
			if (!stack.empty())
			{
				stack.pop_back();
			}
			pending = BraceKind::Other;
			out.push_back('}');
			++i;
			continue;
		}
		if (src[i] == '(')
		{
			++paren;
			out.push_back('(');
			++i;
			continue;
		}
		if (src[i] == ')')
		{
			if (paren > 0)
			{
				--paren;
			}
			out.push_back(')');
			++i;
			continue;
		}
		if (src[i] == ';' && paren == 0)
		{
			pending = BraceKind::Other;
		}
		out.push_back(src[i]);
		++i;
	}
	return out;
}

void skipSpaces(const std::string& s, std::size_t& i)
{
	while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) != 0)
	{
		++i;
	}
}

void skipSpacesBack(const std::string& s, std::size_t& i)
{
	while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1])) != 0)
	{
		--i;
	}
}

// Skip // comments, /* */ comments, "strings", and 'chars'. Returns the
// next index that is in ordinary code, or s.size().
std::size_t skipNonCode(const std::string& s, std::size_t i)
{
	if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/')
	{
		i += 2;
		while (i < s.size() && s[i] != '\n')
		{
			++i;
		}
		return i;
	}
	if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '*')
	{
		i += 2;
		while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
		{
			++i;
		}
		return i + 1 < s.size() ? i + 2 : s.size();
	}
	if (s[i] == '"' || s[i] == '\'')
	{
		const char q = s[i];
		++i;
		while (i < s.size() && s[i] != q)
		{
			if (s[i] == '\\' && i + 1 < s.size())
			{
				i += 2;
			}
			else
			{
				++i;
			}
		}
		return i < s.size() ? i + 1 : s.size();
	}
	return i;
}

std::size_t matchingCloseParen(const std::string& s, std::size_t open)
{
	int depth = 1;
	std::size_t i = open + 1;
	while (i < s.size() && depth > 0)
	{
		const std::size_t next = skipNonCode(s, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (s[i] == '(')
		{
			++depth;
		}
		else if (s[i] == ')')
		{
			--depth;
			if (depth == 0)
			{
				return i;
			}
		}
		++i;
	}
	return std::string::npos;
}

bool looksLikeDeclarator(const std::string& s, std::size_t namePos)
{
	std::size_t i = namePos;
	skipSpacesBack(s, i);
	while (i > 0 && (s[i - 1] == '*' || std::isspace(static_cast<unsigned char>(s[i - 1])) != 0))
	{
		--i;
	}
	if (i == 0)
	{
		return false;
	}
	return isIdentCont(s[i - 1]);
}

bool isDefinitionAfterClose(const std::string& s, std::size_t close)
{
	std::size_t i = close + 1;
	skipSpaces(s, i);
	return i < s.size() && s[i] == '{';
}

void collectCallAndDefinedNames(const std::string& src, std::set<std::string>& undeclaredCalls)
{
	std::set<std::string> declared;
	std::set<std::string> called;
	std::size_t i = 0;
	while (i < src.size())
	{
		const std::size_t next = skipNonCode(src, i);
		if (next != i)
		{
			i = next;
			continue;
		}
		if (!isIdentStart(src[i]))
		{
			++i;
			continue;
		}
		const std::size_t start = i;
		++i;
		while (i < src.size() && isIdentCont(src[i]))
		{
			++i;
		}
		std::size_t after = i;
		skipSpaces(src, after);
		if (after >= src.size() || src[after] != '(')
		{
			continue;
		}
		const std::string name = src.substr(start, i - start);
		if (isSkippedCallName(name))
		{
			i = after + 1;
			continue;
		}
		const std::size_t close = matchingCloseParen(src, after);
		if (looksLikeDeclarator(src, start) || (close != std::string::npos && isDefinitionAfterClose(src, close)))
		{
			declared.insert(name);
		}
		else
		{
			called.insert(name);
		}
		i = after + 1;
	}
	for (const auto& name: called)
	{
		if (declared.find(name) == declared.end())
		{
			undeclaredCalls.insert(name);
		}
	}
}

std::vector<std::string> scrapeTypeBlocks(const std::string& src)
{
	std::vector<std::string> out;
	std::istringstream iss(src);
	std::string line;
	std::string accum;
	bool capturing = false;
	while (std::getline(iss, line))
	{
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		if (!capturing)
		{
			const auto first = line.find_first_not_of(" \t");
			if (first == std::string::npos || first > 3)
			{
				continue;
			}
			const std::string rest = line.substr(first);
			const bool isTypedef = rest.compare(0, 7, "typedef") == 0 && (rest.size() == 7 || !isIdentCont(rest[7]));
			const bool isStruct = rest.compare(0, 6, "struct") == 0 && (rest.size() == 6 || !isIdentCont(rest[6]));
			if (!isTypedef && !isStruct)
			{
				continue;
			}
			capturing = true;
			accum.clear();
		}
		if (!accum.empty())
		{
			accum += '\n';
		}
		accum += line;
		if (line.find(';') != std::string::npos)
		{
			out.push_back(accum);
			capturing = false;
			accum.clear();
		}
	}
	if (capturing && !accum.empty())
	{
		out.push_back(accum);
	}
	return out;
}

std::string readFileIfEmpty(const std::string& path, const std::string& cSource)
{
	if (!cSource.empty() || path.empty())
	{
		return cSource;
	}
	std::ifstream in(path);
	if (!in)
	{
		return {};
	}
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool writeTextFile(const std::string& path, const std::string& text)
{
	std::ofstream out(path, std::ios::trunc);
	if (!out)
	{
		return false;
	}
	out << text;
	return static_cast<bool>(out);
}

} // anonymous namespace

SemanticDetectionMap buildSemanticDetectionMap(
	const container_detect::ContainerDetector::DetectionMap& containers,
	const std::vector<std::pair<std::string, algo_recover::AlgorithmResult>>& algos,
	const std::vector<std::pair<std::string, algo_recover::IdiomResult>>& idioms,
	const sort_detect::SortDetector::DetectionMap& sorts,
	const concurrency_detect::ConcurrencyModel& concurrency,
	const std::string& outputLang)
{
	SemanticDetectionMap map;
	const bool emitCHints = common::isCOutputLang(outputLang);

	for (const auto& [fnName, result]: containers)
	{
		const std::string label = !result.emittedType.empty() ? result.emittedType : result.kindName();
		auto detection = makeDetection("container", label, result.confidence, result.toString());
		if (emitCHints)
		{
			detection.cHint = result.cHint();
			detection.cElemBytes = result.elementType.byteWidth;
		}
		appendDetection(map, fnName, std::move(detection));
	}

	for (const auto& [fnName, result]: algos)
	{
		appendDetection(
			map, fnName, makeDetection("algorithm", result.kindName(), result.confidence, result.toString()));
	}

	for (const auto& [fnName, result]: idioms)
	{
		for (const auto& label: result.exportLabels())
		{
			appendDetection(map, fnName, makeDetection("algorithm", label, result.confidence, result.toString()));
		}
	}

	for (const auto& [fnName, result]: sorts)
	{
		appendDetection(
			map, fnName, makeDetection("sort", result.algorithmName(), result.confidence, result.toString()));
	}

	collectConcurrencyDetections(concurrency, map);
	return map;
}

void mergeSemanticDetectionsIntoConfig(config::Config& config, const SemanticDetectionMap& detections)
{
	for (const auto& [fnName, dets]: detections)
	{
		if (dets.empty())
		{
			continue;
		}

		common::Function key(fnName);
		auto it = config.functions.find(key);
		if (it == config.functions.end())
		{
			continue;
		}

		common::Function fn = *it;
		config.functions.erase(it);
		fn.semanticDetections = dets;
		config.functions.insert(std::move(fn));
	}
}

void injectSemanticCommentsIntoOutput(const config::Config& config, std::string* outString)
{
	std::vector<std::string> lines;
	const std::string& outPath = config.parameters.getOutputFile();

	if (!outPath.empty())
	{
		std::ifstream in(outPath);
		if (in)
		{
			std::string line;
			while (std::getline(in, line))
			{
				lines.push_back(line);
			}
		}
	}
	else if (outString && !outString->empty())
	{
		std::istringstream iss(*outString);
		std::string line;
		while (std::getline(iss, line))
		{
			lines.push_back(line);
		}
	}

	if (lines.empty())
	{
		return;
	}

	injectSemanticCommentsIntoLines(lines, config);

	if (!outPath.empty())
	{
		std::ofstream out(outPath, std::ios::trunc);
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			out << lines[i];
			if (i + 1 < lines.size())
			{
				out << '\n';
			}
		}
	}

	if (outString)
	{
		std::ostringstream oss;
		for (std::size_t i = 0; i < lines.size(); ++i)
		{
			oss << lines[i];
			if (i + 1 < lines.size())
			{
				oss << '\n';
			}
		}
		*outString = oss.str();
	}
}

void exportSemanticRecovery(config::Config& config, const SemanticDetectionMap& detections, std::string* outString)
{
	if (detections.empty())
	{
		return;
	}

	mergeSemanticDetectionsIntoConfig(config, detections);

	if (!config.parameters.getOutputConfigFile().empty())
	{
		config.generateJsonFile();
	}

	injectSemanticCommentsIntoOutput(config, outString);
}

void maybeWriteBuildableSidecars(const std::string& outputCPath, const std::string& cSource)
{
	if (!emitBuildableEnabled() || outputCPath.empty())
	{
		return;
	}

	const std::string src = readFileIfEmpty(outputCPath, cSource);
	const std::string stem = outputStem(outputCPath);
	const std::string headerPath = stem + ".h";
	const std::string stubsPath = stem + "_stubs.c";
	const std::string buildablePath = stem + ".buildable.c";
	const std::string headerName = pathBasename(headerPath);
	const std::string guard = includeGuardFromHeaderName(headerName);

	std::set<std::string> undeclared;
	collectCallAndDefinedNames(src, undeclared);
	std::set<std::string> libcHeaders;
	for (auto it = undeclared.begin(); it != undeclared.end();)
	{
		if (const char* hdr = libcHeaderForName(*it))
		{
			libcHeaders.insert(hdr);
			it = undeclared.erase(it);
		}
		else if (isCrtCallName(*it) || isPthreadName(*it) || isAsmIntrinsicName(*it))
		{
			it = undeclared.erase(it);
		}
		else
		{
			++it;
		}
	}

	std::ostringstream header;
	header << "#ifndef " << guard << "\n#define " << guard << "\n\n";
	header << "#include <stdint.h>\n";
	header << "#include <stddef.h>\n";
	header << "#include <stdbool.h>\n";
	header << "\n";
	header << "uint64_t __readUndefQword(void);\n";
	header << "uint64_t __readfsqword(unsigned long);\n";
	header << "void __asm_hlt(void);\n";
	header << "void *__asm_rep_stosq_memset(void *, int, unsigned long);\n";

	const auto typeBlocks = scrapeTypeBlocks(src);
	if (!typeBlocks.empty())
	{
		header << '\n';
		for (const auto& block: typeBlocks)
		{
			header << block << '\n';
		}
	}

	if (!undeclared.empty())
	{
		header << '\n';
		for (const auto& name: undeclared)
		{
			header << "int " << name << "(void);\n";
		}
	}

	header << "\n#endif\n";
	writeTextFile(headerPath, header.str());

	std::ostringstream stubs;
	stubs << "#include \"" << headerName << "\"\n";
	stubs << "#include <string.h>\n\n";
	stubs << "void __retdec_stub(void) {}\n";
	writeHelperStubs(stubs);
	writePrototypeBodies(stubs, src);
	for (const auto& name: undeclared)
	{
		stubs << "int " << name << "(void) { return 0; }\n";
	}
	if (!sourceDefinesMain(src))
	{
		stubs << "\nint main(void) { return 0; }\n";
	}
	writeTextFile(stubsPath, stubs.str());

	std::ostringstream buildable;
	const std::string includeLine = "#include \"" + headerName + "\"";
	if (src.find(includeLine) == std::string::npos)
	{
		buildable << includeLine << '\n';
	}
	buildable << "#include <string.h>\n";
	const bool hasChk = src.find("__printf_chk") != std::string::npos || src.find("__fprintf_chk") != std::string::npos;
	if (!hasChk)
	{
		buildable << "#include <stdio.h>\n";
	}
	else
	{
		buildable << "int puts(const char *);\n";
		buildable << "int putchar(int);\n";
		buildable << "int putc(int, void *);\n";
		buildable << "int printf(const char *, ...);\n";
		buildable << "#define stdout ((void *)0)\n";
	}
	for (const auto& hdr: libcHeaders)
	{
		if (hdr != "string.h" && hdr != "stdio.h")
		{
			buildable << "#include <" << hdr << ">\n";
		}
	}
	writeLibcArityMacros(buildable);
	std::set<std::string> asmNames;
	collectAsmIntrinsicNames(src, asmNames);
	for (const auto& name: asmNames)
	{
		buildable << "#define " << name << "(...) ((uint64_t)0)\n";
	}
	for (const auto& name: undeclared)
	{
		buildable << "#define " << name << "(...) ((" << name << ")())\n";
	}
	const std::string sidecarSrc = stripSidecarSystemIncludes(src);
	buildable << rewriteOrphanBreakContinue(injectUndeclaredTemps(sidecarSrc));
	if (!src.empty() && src.back() != '\n')
	{
		buildable << '\n';
	}
	buildable << "\n/* RETDEC_BUILDABLE_STUBS */\n";
	for (const auto& name: undeclared)
	{
		buildable << "#undef " << name << "\n";
	}
	writeHelperStubs(buildable);
	writePrototypeBodies(buildable, src);
	for (const auto& name: undeclared)
	{
		buildable << "int " << name << "(void) { return 0; }\n";
	}
	if (!sourceDefinesMain(src))
	{
		buildable << "int main(void) { return 0; }\n";
	}
	writeTextFile(buildablePath, buildable.str());
}

} // namespace analysis
} // namespace retdec
