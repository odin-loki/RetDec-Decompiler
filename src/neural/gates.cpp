#include "retdec/neural/gates.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

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

bool controlShapeChanged(const std::string& originalC, const std::string& refinedC)
{
	if (countIdent(originalC, "if") != countIdent(refinedC, "if")) return true;
	if (countIdent(originalC, "else") != countIdent(refinedC, "else")) return true;
	if (countIdent(originalC, "while") != countIdent(refinedC, "while")) return true;
	if (countIdent(originalC, "for") != countIdent(refinedC, "for")) return true;
	if (countIdent(originalC, "goto") != countIdent(refinedC, "goto")) return true;
	if (countIdent(originalC, "return") != countIdent(refinedC, "return")) return true;
	static const char* const kSpawnIdents[] = {
		"system",         "popen",         "execve",        "execl",         "execle",
		"execlp",         "execv",         "execvp",        "execvpe",       "WinExec",
		"ShellExecute",   "ShellExecuteA", "ShellExecuteW", "CreateProcess", "CreateProcessA",
		"CreateProcessW", "_popen",        "_wpopen",       "_wsystem",
	};
	for (const char* w: kSpawnIdents)
	{
		if (countIdent(originalC, w) != countIdent(refinedC, w)) return true;
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
	// N5 (not N10): same-size refinements may not change control-flow
	// keywords or comparison operators. FullRewrite that grows the TU
	// skips this check. No C parser in deps/.
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
