#include "retdec/neural/gates.h"

#include "retdec/utils/c_source_scan.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef RETDEC_HAS_TREE_SITTER
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-c.h>
#endif

#include <forge/os/fs.hpp>
#include <forge/os/process.hpp>

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

/// A private (0700 / owner-only DACL) directory for the gate's source file.
/// Empty when none could be made.
fs::path createUniqueTempDir()
{
	auto dir = forge::os::fs::make_temp_dir("retdec_gate_");
	if (!dir) return {};
	return fs::path(dir.value);
}

/// How long the gate waits for the compiler before killing it.
///
/// The source it is given is model output, so the wait has to be bounded by
/// something other than that source. RETDEC_NEURAL_GATE_CC_TIMEOUT_MS moves
/// it, within reason -- the tests use a short one so a case that checks the
/// timeout does not take twenty seconds to check it.
unsigned gateCompilerTimeoutMs()
{
	const char* env = std::getenv("RETDEC_NEURAL_GATE_CC_TIMEOUT_MS");
	if (env && env[0])
	{
		const long v = std::strtol(env, nullptr, 10);
		if (v >= 100 && v <= 600000) return static_cast<unsigned>(v);
	}
	return 20000;
}

/// Diagnostics beyond this are dropped: they go back into a model prompt.
constexpr std::uint64_t kGateMaxDiagnostics = 1u << 20;

/// Runs `cc -fsyntax-only -w <src>` through Forge's process layer: argv, no
/// shell; stdin from the null device; stdout and stderr captured together
/// into `diagnostics`; and bounded -- the C handed to this compiler is model
/// output, so anything that makes the preprocessor block (an #include of a
/// FIFO, a macro expansion bomb) would otherwise hang the decompiler. On the
/// timeout Forge kills the whole process tree (process group / Job Object),
/// so the cc1 the driver started goes too, not only the driver.
bool spawnSyntaxOnlyCompiler(const char* cc, const fs::path& src, std::string* diagnostics)
{
	forge::os::spawn_spec spec;
	spec.argv = {cc, "-fsyntax-only", "-w", src.string()};
	spec.merge_stderr = true;
	spec.capture_stdout = true;
	spec.max_output = kGateMaxDiagnostics;
	spec.timeout_ms = gateCompilerTimeoutMs();

	const auto r = forge::os::spawn_wait(spec);
	if (!r) return false;
	if (diagnostics) *diagnostics = r.value.out;
	return !r.value.timed_out && !r.value.cancelled && r.value.exit_code == 0;
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

/// Returns the first #include directive whose header name is not a plain
/// relative name, or an empty string when there is none.
///
/// The compile gate execs a real compiler on model output, and a compiler
/// honours #include. gcc quotes the offending source lines of any file it
/// includes into its diagnostics; compileSyntaxOnly returns those diagnostics
/// verbatim, runTieredRefine puts them in retry.compilerDiagnostics, and
/// buildRefinementPrompt embeds that in the next prompt. So
/// `#include "/etc/shadow"` in model output got that file read back to the
/// model. `#include </etc/shadow>` does the same: an absolute path works
/// through the angle form too.
///
/// A plain relative name -- <stdint.h>, <sys/types.h> -- names a system
/// header and reads nothing secret, so those still pass. Anything absolute,
/// anything with a ".." component, and anything outside the character set a
/// header name uses is refused.
std::string firstUnsafeInclude(const std::string& sourceC)
{
	// Splice line continuations first, the way the preprocessor does, so that
	// `#inc\` + newline + `lude "..."` cannot slip past a line-based scan.
	std::string spliced;
	spliced.reserve(sourceC.size());
	for (std::size_t i = 0; i < sourceC.size(); ++i)
	{
		if (sourceC[i] == '\\' && i + 1 < sourceC.size() && sourceC[i + 1] == '\n')
		{
			++i;
			continue;
		}
		if (sourceC[i] == '\\' && i + 2 < sourceC.size() && sourceC[i + 1] == '\r' && sourceC[i + 2] == '\n')
		{
			i += 2;
			continue;
		}
		spliced.push_back(sourceC[i]);
	}

	const auto nameIsPlain = [](const std::string& name) {
		if (name.empty()) return false;
		if (name.front() == '/' || name.front() == '\\') return false;
		if (name.find("..") != std::string::npos) return false;
		if (name.size() > 1 && name[1] == ':') return false; // C:\...
		for (const char ch: name)
		{
			const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
						 || ch == '_' || ch == '.' || ch == '+' || ch == '-' || ch == '/';
			if (!ok) return false;
		}
		return true;
	};

	std::size_t pos = 0;
	while (pos <= spliced.size())
	{
		std::size_t eol = spliced.find('\n', pos);
		if (eol == std::string::npos) eol = spliced.size();
		const std::string line = spliced.substr(pos, eol - pos);
		pos = eol + 1;

		std::size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			++i;
		if (i >= line.size() || line[i] != '#') continue;
		++i;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			++i;
		if (line.compare(i, 7, "include") != 0) continue;
		i += 7;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
			++i;
		if (i >= line.size()) return line;

		char closer = '\0';
		if (line[i] == '<')
			closer = '>';
		else if (line[i] == '"')
			closer = '"';
		else
			return line; // computed include: not a name this can check

		const std::size_t nameStart = i + 1;
		const std::size_t nameEnd = line.find(closer, nameStart);
		if (nameEnd == std::string::npos) return line;
		if (!nameIsPlain(line.substr(nameStart, nameEnd - nameStart))) return line;
	}
	return {};
}

bool tryCompileCheck(const std::string& sourceC, std::string* diagnostics)
{
	if (diagnostics) diagnostics->clear();

	const std::string bad = firstUnsafeInclude(sourceC);
	if (!bad.empty())
	{
		if (diagnostics)
		{
			*diagnostics =
				"gate: refusing to compile a source that includes a "
				"file by path: "
				+ bad + "\n";
		}
		return false;
	}

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

	return spawnSyntaxOnlyCompiler(gateCompiler(), src, diagnostics);
}

bool tryCompileCheck(const std::string& sourceC)
{
	return tryCompileCheck(sourceC, nullptr);
}

/// Replace the contents of comments and string/character literals with spaces,
/// keeping length and newlines so offsets and line numbers still line up.
///
/// The textual fallback below counts keywords and comparison operators in raw
/// source. Doing that over comments and literals is wrong in both directions:
/// adding an explanatory comment that happens to contain "if" or "while" looks
/// like a control-flow change and gets a benign refinement rejected, and -- the
/// dangerous direction -- a refinement that introduces a real system() call
/// while dropping a `/* system */` comment leaves the raw count unchanged, so
/// the spawn check cancels out and the call passes the gate.
///
/// Only the fallback needs this. When tree-sitter is available the shape comes
/// from the parse tree, which never sees comments or literal contents.
///
/// The scanner itself lives in retdec/utils/c_source_scan.h, where it is proved
/// not to index outside either buffer for any input
/// (tests/verification/source_scan_proof.cpp). It reads one character ahead,
/// which is where this shape of loop usually goes wrong.
std::string blankNonCode(const std::string& s)
{
	if (s.empty()) return {};

	std::string out(s.size(), '\0');
	retdec::utils::source_scan::blankNonCode(s.data(), s.size(), &out[0]);
	return out;
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
	if (op == "==")
		++s.cmp.eq;
	else if (op == "!=")
		++s.cmp.ne;
	else if (op == "<=")
		++s.cmp.le;
	else if (op == ">=")
		++s.cmp.ge;
	else if (op == "<")
		++s.cmp.lt;
	else if (op == ">")
		++s.cmp.gt;
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
	if (std::strcmp(ty, "if_statement") == 0)
		++s.ifN;
	else if (std::strcmp(ty, "else_clause") == 0)
		++s.elseN;
	else if (std::strcmp(ty, "while_statement") == 0)
		++s.whileN;
	else if (std::strcmp(ty, "for_statement") == 0)
		++s.forN;
	else if (std::strcmp(ty, "goto_statement") == 0)
		++s.gotoN;
	else if (std::strcmp(ty, "return_statement") == 0)
		++s.returnN;
	else if (std::strcmp(ty, "binary_expression") == 0)
	{
		TSNode op = ts_node_child_by_field_name(n, "operator", 8);
		if (!ts_node_is_null(op)) addCmpOp(s, nodeText(src, op));
	}
	else if (std::strcmp(ty, "call_expression") == 0)
	{
		TSNode fn = ts_node_child_by_field_name(n, "function", 8);
		if (!ts_node_is_null(fn) && std::strcmp(ts_node_type(fn), "identifier") == 0) addSpawn(s, nodeText(src, fn));
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

bool astSpawnChanged(const AstShape& a, const AstShape& b)
{
	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (a.spawn[i] != b.spawn[i]) return true;
	}
	return false;
}

bool astControlFlowChanged(const AstShape& a, const AstShape& b)
{
	if (a.ifN != b.ifN || a.elseN != b.elseN || a.whileN != b.whileN || a.forN != b.forN || a.gotoN != b.gotoN
		|| a.returnN != b.returnN)
		return true;
	return !(a.cmp == b.cmp);
}

#endif

/// Did the refinement add or remove a process-spawning call?
///
/// Asked on EVERY refinement, whatever its size. This used to be one half of
/// controlShapeChanged, which only ran when the refinement was within 4x the
/// original's size -- so a model could smuggle in a system() call simply by
/// padding its output past that threshold, defeating the spawn-call rejection
/// docs/CLAIMS.md advertises under C-NEURAL and C-N14. The control-flow half
/// legitimately varies when a rewrite adds statements; this half never does.
bool spawnCallsChanged(const std::string& originalC, const std::string& refinedC, bool& usedParser)
{
	usedParser = false;
#ifdef RETDEC_HAS_TREE_SITTER
	{
		AstShape a;
		AstShape b;
		if (fillAstShape(originalC, a) && fillAstShape(refinedC, b))
		{
			usedParser = true;
			return astSpawnChanged(a, b);
		}
	}
#endif
	const std::string original = blankNonCode(originalC);
	const std::string refined = blankNonCode(refinedC);

	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (countIdent(original, kSpawnIdents[i]) != countIdent(refined, kSpawnIdents[i])) return true;
	}
	return false;
}

bool controlShapeChanged(const std::string& originalC, const std::string& refinedC, bool& usedParser)
{
	usedParser = false;
#ifdef RETDEC_HAS_TREE_SITTER
	AstShape a;
	AstShape b;
	if (fillAstShape(originalC, a) && fillAstShape(refinedC, b))
	{
		usedParser = true;
		return astControlFlowChanged(a, b) || astSpawnChanged(a, b);
	}
#endif
	// Fallback: count over code only.  See blankNonCode.
	const std::string original = blankNonCode(originalC);
	const std::string refined = blankNonCode(refinedC);

	if (countIdent(original, "if") != countIdent(refined, "if")) return true;
	if (countIdent(original, "else") != countIdent(refined, "else")) return true;
	if (countIdent(original, "while") != countIdent(refined, "while")) return true;
	if (countIdent(original, "for") != countIdent(refined, "for")) return true;
	if (countIdent(original, "goto") != countIdent(refined, "goto")) return true;
	if (countIdent(original, "return") != countIdent(refined, "return")) return true;
	for (int i = 0; kSpawnIdents[i]; ++i)
	{
		if (countIdent(original, kSpawnIdents[i]) != countIdent(refined, kSpawnIdents[i])) return true;
	}
	if (!(countCmpOps(original) == countCmpOps(refined))) return true;
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
	return std::string("compile=") + (compile == GateResult::Pass ? "pass" : "fail") + " structural="
		 + (structural == GateResult::Pass ? "pass" : "fail") + (structuralUsedParser ? "" : "(text-fallback)")
		 + " differential=" + (differential == GateResult::Pass ? "pass" : "fail");
}

bool hasCParserSupport()
{
#ifdef RETDEC_HAS_TREE_SITTER
	return true;
#else
	return false;
#endif
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
	// The spawn-call check is unconditional. A refinement more than 4x the
	// original's size used to skip the whole structural gate, so padding the
	// output was enough to smuggle a system() call past the one protection
	// docs/CLAIMS.md names by identifier.
	{
		bool usedParser = false;
		const bool spawnChanged = spawnCallsChanged(originalC, refinedC, usedParser);
		report.structuralUsedParser = usedParser;
		if (spawnChanged)
		{
			report.structural = GateResult::FailStructural;
			return report;
		}
	}

	// The control-flow half stays size-gated: a FullRewrite that legitimately
	// grows the translation unit does change the statement counts, and failing
	// it there would reject every such refinement.
	const bool similarSize =
		originalC.size() > 16 && refinedC.size() * 4 > originalC.size() && originalC.size() * 4 > refinedC.size();
	if (similarSize)
	{
		bool usedParser = false;
		const bool changed = controlShapeChanged(originalC, refinedC, usedParser);
		report.structuralUsedParser = usedParser;
		if (changed)
		{
			report.structural = GateResult::FailStructural;
			return report;
		}
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
