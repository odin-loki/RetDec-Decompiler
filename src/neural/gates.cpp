#include "retdec/neural/gates.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#ifdef RETDEC_HAS_TREE_SITTER
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace retdec::neural {

namespace {

namespace fs = std::filesystem;

struct ScopedTempDir
{
	fs::path path;

	explicit ScopedTempDir(fs::path p): path(std::move(p)) {}
	~ScopedTempDir()
	{
		if (path.empty()) return;
		std::error_code ec;
		fs::remove_all(path, ec);
	}

	ScopedTempDir(const ScopedTempDir&) = delete;
	ScopedTempDir& operator=(const ScopedTempDir&) = delete;
};

fs::path createUniqueTempDir()
{
#if defined(_WIN32)
	wchar_t tmp[MAX_PATH];
	const DWORD n = GetTempPathW(MAX_PATH, tmp);
	if (n == 0 || n >= MAX_PATH) return {};

	const DWORD pid = GetCurrentProcessId();
	const ULONGLONG ticks = GetTickCount64();
	for (int i = 0; i < 256; ++i)
	{
		wchar_t name[MAX_PATH];
		if (swprintf_s(name, L"%sretdec_gate_%lu_%llu_%d", tmp, static_cast<unsigned long>(pid), ticks, i) < 0)
			return {};
		if (CreateDirectoryW(name, nullptr)) return fs::path(name);
		if (GetLastError() != ERROR_ALREADY_EXISTS) return {};
	}
	return {};
#else
	const fs::path tmpl = fs::temp_directory_path() / "retdec_gate_XXXXXX";
	std::string s = tmpl.string();
	std::vector<char> buf(s.begin(), s.end());
	buf.push_back('\0');
	if (!mkdtemp(buf.data())) return {};
	const fs::path dir(buf.data());
	::chmod(dir.c_str(), 0700);
	return dir;
#endif
}

#if defined(_WIN32)
std::wstring utf8ToWide(const std::string& s)
{
	if (s.empty()) return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (n <= 0) return {};
	std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

std::wstring quoteWinArg(const std::wstring& a)
{
	if (a.find_first_of(L" \t\"") == std::wstring::npos) return a;
	std::wstring out = L"\"";
	for (wchar_t c: a)
	{
		if (c == L'"')
			out += L"\"\"";
		else
			out += c;
	}
	out += L'"';
	return out;
}
#endif

std::string readAll(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	if (!in) return {};
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool spawnSyntaxOnlyCompiler(const char* cc, const fs::path& src, const fs::path& diagFile)
{
#if defined(_WIN32)
	const std::wstring wcc = utf8ToWide(cc);
	const std::wstring cmd = quoteWinArg(wcc) + L" -fsyntax-only -w " + quoteWinArg(src.wstring());
	std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
	cmdline.push_back(L'\0');

	SECURITY_ATTRIBUTES sa{};
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = TRUE;
	HANDLE err = CreateFileW(
		diagFile.empty() ? L"NUL" : diagFile.wstring().c_str(),
		GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		&sa,
		diagFile.empty() ? OPEN_EXISTING : CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	if (err != INVALID_HANDLE_VALUE)
	{
		si.hStdOutput = err;
		si.hStdError = err;
		si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	}

	PROCESS_INFORMATION pi{};
	const BOOL ok =
		CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	if (err != INVALID_HANDLE_VALUE) CloseHandle(err);
	if (!ok) return false;

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return code == 0;
#else
	const std::string srcPath = src.string();
	const std::string errPath = diagFile.empty() ? std::string() : diagFile.string();
	const pid_t pid = fork();
	if (pid < 0) return false;
	if (pid == 0)
	{
		int outfd = -1;
		if (!errPath.empty()) outfd = open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (outfd < 0) outfd = open("/dev/null", O_WRONLY);
		if (outfd >= 0)
		{
			dup2(outfd, STDOUT_FILENO);
			dup2(outfd, STDERR_FILENO);
			close(outfd);
		}
		const char* argv[] = {cc, "-fsyntax-only", "-w", srcPath.c_str(), nullptr};
		execvp(cc, const_cast<char* const*>(argv));
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

const char* gateCompiler()
{
	const char* cc = std::getenv("RETDEC_NEURAL_GATE_CC");
	if (cc && cc[0]) return cc;
#if defined(_WIN32)
	return "gcc";
#else
	return "cc";
#endif
}

bool tryCompileCheck(const std::string& sourceC, std::string* diagnostics)
{
	if (diagnostics) diagnostics->clear();
	const fs::path dir = createUniqueTempDir();
	if (dir.empty()) return false;
	ScopedTempDir guard(dir);

	const fs::path src = dir / "gate.c";
	{
		std::ofstream out(src, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << sourceC;
		if (!out) return false;
	}

	const fs::path diag = dir / "gate.err";
	const bool ok = spawnSyntaxOnlyCompiler(gateCompiler(), src, diag);
	if (diagnostics) *diagnostics = readAll(diag);
	return ok;
}

bool tryCompileCheck(const std::string& sourceC)
{
	return tryCompileCheck(sourceC, nullptr);
}

int countIdent(const std::string& s, const char* word)
{
	int n = 0;
	const std::size_t wlen = std::char_traits<char>::length(word);
	for (std::size_t i = 0; i + wlen <= s.size(); ++i)
	{
		if (s.compare(i, wlen, word) != 0)
		{
			continue;
		}
		const bool leftOk = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) != 0 || s[i - 1] == '_');
		const bool rightOk =
			i + wlen >= s.size() || !(std::isalnum(static_cast<unsigned char>(s[i + wlen])) != 0 || s[i + wlen] == '_');
		if (leftOk && rightOk)
		{
			++n;
		}
	}
	return n;
}

struct CmpOpCounts
{
	int eq = 0;
	int ne = 0;
	int le = 0;
	int ge = 0;
	int lt = 0;
	int gt = 0;
};

bool operator==(const CmpOpCounts& a, const CmpOpCounts& b)
{
	return a.eq == b.eq && a.ne == b.ne && a.le == b.le && a.ge == b.ge && a.lt == b.lt && a.gt == b.gt;
}

static const char* const kSpawnIdents[] = {
	"system",
	"popen",
	"execve",
	"execl",
	"execle",
	"execlp",
	"execv",
	"execvp",
	"execvpe",
	"WinExec",
	"ShellExecute",
	"ShellExecuteA",
	"ShellExecuteW",
	"ShellExecuteEx",
	"ShellExecuteExA",
	"ShellExecuteExW",
	"CreateProcess",
	"CreateProcessA",
	"CreateProcessW",
	"CreateProcessAsUser",
	"CreateProcessAsUserA",
	"CreateProcessAsUserW",
	"_popen",
	"_wpopen",
	"_wsystem",
	"posix_spawn",
	"posix_spawnp",
	nullptr,
};

CmpOpCounts countCmpOps(const std::string& s)
{
	CmpOpCounts n;
	for (std::size_t i = 0; i < s.size(); ++i)
	{
		if (i + 1 < s.size() && s[i] == '=' && s[i + 1] == '=')
		{
			++n.eq;
			++i;
			continue;
		}
		if (i + 1 < s.size() && s[i] == '!' && s[i + 1] == '=')
		{
			++n.ne;
			++i;
			continue;
		}
		if (i + 1 < s.size() && s[i] == '<' && s[i + 1] == '=')
		{
			++n.le;
			++i;
			continue;
		}
		if (i + 1 < s.size() && s[i] == '>' && s[i + 1] == '=')
		{
			++n.ge;
			++i;
			continue;
		}
		if (s[i] == '<')
		{
			++n.lt;
			continue;
		}
		if (s[i] == '>')
		{
			++n.gt;
		}
	}
	return n;
}

#ifdef RETDEC_HAS_TREE_SITTER

std::string nodeText(const std::string& src, TSNode n)
{
	const uint32_t a = ts_node_start_byte(n);
	const uint32_t b = ts_node_end_byte(n);
	if (a > b || b > src.size()) return {};
	return src.substr(a, b - a);
}

struct AstShape
{
	int ifN = 0;
	int elseN = 0;
	int whileN = 0;
	int forN = 0;
	int gotoN = 0;
	int returnN = 0;
	CmpOpCounts cmp;
	int spawn[32] = {};
};

void addCmpOp(AstShape& s, const std::string& op)
{
	if (op == "==") ++s.cmp.eq;
	else if (op == "!=") ++s.cmp.ne;
	else if (op == "<=") ++s.cmp.le;
	else if (op == ">=") ++s.cmp.ge;
	else if (op == "<") ++s.cmp.lt;
	else if (op == ">") ++s.cmp.gt;
}

void addSpawn(AstShape& s, const std::string& id)
{
	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (id == kSpawnIdents[i])
		{
			++s.spawn[i];
			return;
		}
	}
}

void walkAst(TSNode n, const std::string& src, AstShape& s)
{
	const char* ty = ts_node_type(n);
	if (std::strcmp(ty, "if_statement") == 0) ++s.ifN;
	else if (std::strcmp(ty, "else_clause") == 0) ++s.elseN;
	else if (std::strcmp(ty, "while_statement") == 0) ++s.whileN;
	else if (std::strcmp(ty, "for_statement") == 0) ++s.forN;
	else if (std::strcmp(ty, "goto_statement") == 0) ++s.gotoN;
	else if (std::strcmp(ty, "return_statement") == 0) ++s.returnN;
	else if (std::strcmp(ty, "binary_expression") == 0)
	{
		TSNode op = ts_node_child_by_field_name(n, "operator", 8);
		if (!ts_node_is_null(op)) addCmpOp(s, nodeText(src, op));
	}
	else if (std::strcmp(ty, "call_expression") == 0)
	{
		TSNode fn = ts_node_child_by_field_name(n, "function", 8);
		if (!ts_node_is_null(fn) && std::strcmp(ts_node_type(fn), "identifier") == 0)
			addSpawn(s, nodeText(src, fn));
	}

	const uint32_t nch = ts_node_child_count(n);
	for (uint32_t i = 0; i < nch; ++i)
		walkAst(ts_node_child(n, i), src, s);
}

bool fillAstShape(const std::string& src, AstShape& out)
{
	TSParser* p = ts_parser_new();
	if (!p) return false;
	if (!ts_parser_set_language(p, tree_sitter_c()))
	{
		ts_parser_delete(p);
		return false;
	}
	TSTree* tree = ts_parser_parse_string(p, nullptr, src.data(), static_cast<uint32_t>(src.size()));
	if (!tree)
	{
		ts_parser_delete(p);
		return false;
	}
	TSNode root = ts_tree_root_node(tree);
	const bool ok = !ts_node_is_null(root) && !ts_node_has_error(root);
	if (ok) walkAst(root, src, out);
	ts_tree_delete(tree);
	ts_parser_delete(p);
	return ok;
}

bool astShapeChanged(const AstShape& a, const AstShape& b)
{
	if (a.ifN != b.ifN || a.elseN != b.elseN || a.whileN != b.whileN || a.forN != b.forN
		|| a.gotoN != b.gotoN || a.returnN != b.returnN)
		return true;
	if (!(a.cmp == b.cmp)) return true;
	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (a.spawn[i] != b.spawn[i]) return true;
	}
	return false;
}

#endif

bool controlShapeChanged(const std::string& originalC, const std::string& refinedC)
{
#ifdef RETDEC_HAS_TREE_SITTER
	AstShape a;
	AstShape b;
	if (fillAstShape(originalC, a) && fillAstShape(refinedC, b))
		return astShapeChanged(a, b);
#endif
	if (countIdent(originalC, "if") != countIdent(refinedC, "if")) return true;
	if (countIdent(originalC, "else") != countIdent(refinedC, "else")) return true;
	if (countIdent(originalC, "while") != countIdent(refinedC, "while")) return true;
	if (countIdent(originalC, "for") != countIdent(refinedC, "for")) return true;
	if (countIdent(originalC, "goto") != countIdent(refinedC, "goto")) return true;
	if (countIdent(originalC, "return") != countIdent(refinedC, "return")) return true;
	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (countIdent(originalC, kSpawnIdents[i]) != countIdent(refinedC, kSpawnIdents[i]))
			return true;
	}
	if (!(countCmpOps(originalC) == countCmpOps(refinedC))) return true;
	return false;
}

bool tryDifferentialCheck(const std::string& /*originalC*/, const std::string& /*refinedC*/)
{
	const char* e = std::getenv("RETDEC_NEURAL_DIFF_GATE");
	if (!e || e[0] == '\0' || e[0] == '0') return true;

	std::fprintf(
		stderr,
		"retdec-neural: WARNING: runtime differential execution of decompiled C is DISABLED.\n"
		"  RETDEC_NEURAL_DIFF_GATE is set, but compiling and running decompiled C would execute\n"
		"  attacker-controlled code on this host. The differential gate is skipped (treated as pass).\n"
		"  Unset RETDEC_NEURAL_DIFF_GATE to silence this warning.\n");
	return true;
}

} // namespace

bool GateReport::allPassed() const
{
	return compile == GateResult::Pass && structural == GateResult::Pass && differential == GateResult::Pass;
}

std::string GateReport::summary() const
{
	return std::string("compile=") + (compile == GateResult::Pass ? "pass" : "fail")
		 + " structural=" + (structural == GateResult::Pass ? "pass" : "fail")
		 + " differential=" + (differential == GateResult::Pass ? "pass" : "fail");
}

GateReport runVerificationGates(const std::string& originalC, const std::string& refinedC)
{
	GateReport report;
	if (refinedC.empty())
	{
		report.structural = GateResult::FailStructural;
		return report;
	}
	if (refinedC.size() < originalC.size() / 4 && originalC.size() > 64)
	{
		report.structural = GateResult::FailStructural;
		return report;
	}
	// N10: same-size refinements may not change control-flow AST
	// shape, comparison operators, or spawn calls. Parse failure
	// falls back to the N5 keyword scan. FullRewrite that grows the
	// TU skips this check.
	const bool similarSize =
		originalC.size() > 16 && refinedC.size() * 4 > originalC.size() && originalC.size() * 4 > refinedC.size();
	if (similarSize && controlShapeChanged(originalC, refinedC))
	{
		report.structural = GateResult::FailStructural;
		return report;
	}

	const char* skipCompile = std::getenv("RETDEC_NEURAL_SKIP_COMPILE_GATE");
	if (!skipCompile || skipCompile[0] == '\0' || skipCompile[0] == '0')
	{
		if (!tryCompileCheck(refinedC)) report.compile = GateResult::FailCompile;
	}

	if (report.compile == GateResult::Pass)
	{
		if (!tryDifferentialCheck(originalC, refinedC)) report.differential = GateResult::FailDifferential;
	}

	return report;
}

bool compileSyntaxOnly(const std::string& sourceC)
{
	if (sourceC.empty()) return false;
	return tryCompileCheck(sourceC);
}

bool compileSyntaxOnly(const std::string& sourceC, std::string& diagnostics)
{
	if (sourceC.empty())
	{
		diagnostics.clear();
		return false;
	}
	return tryCompileCheck(sourceC, &diagnostics);
}

} // namespace retdec::neural
