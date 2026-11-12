/**
 * @file src/gui/model/decompiler_launch.cpp
 * @brief retdec-decompiler argv / environment / log helpers.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/decompiler_launch.h"

#include "retdec/gui/model/address.h"
#include "retdec/gui/model/cli_tool_paths.h"

#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <format>
#include <functional>
#include <set>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;
namespace json = forge::json;

namespace {

// ─── String helpers ──────────────────────────────────────────────────────────

bool isSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trimView(std::string_view s)
{
	while (!s.empty() && isSpace(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && isSpace(s.back()))
		s.remove_suffix(1);
	return s;
}

std::string trimmed(std::string_view s)
{
	return std::string(trimView(s));
}

std::string lower(std::string_view s)
{
	std::string out(s);
	for (char& c: out)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return out;
}

bool contains(std::string_view hay, std::string_view needle)
{
	return hay.find(needle) != std::string_view::npos;
}

bool containsCI(std::string_view hay, std::string_view needleLower)
{
	return lower(hay).find(needleLower) != std::string::npos;
}

bool startsWithCI(std::string_view s, std::string_view prefixLower)
{
	return s.size() >= prefixLower.size() && lower(s.substr(0, prefixLower.size())) == prefixLower;
}

std::string joinComma(const std::vector<std::string>& v)
{
	std::string out;
	for (std::size_t i = 0; i < v.size(); ++i)
	{
		if (i) out += ',';
		out += v[i];
	}
	return out;
}

bool fileExists(const std::string& p)
{
	std::error_code ec;
	return !p.empty() && fs::exists(fs::path(p), ec);
}

bool isRegularFile(const std::string& p)
{
	std::error_code ec;
	return !p.empty() && fs::is_regular_file(fs::path(p), ec);
}

fs::path absoluteClean(const fs::path& p)
{
	std::error_code ec;
	fs::path a = fs::absolute(p, ec);
	if (ec) a = p;
	a = a.lexically_normal();
	if (a.has_relative_path() && a.filename().empty()) a = a.parent_path(); // "dir/" -> "dir"
	return a;
}

// ─── File reading ────────────────────────────────────────────────────────────

/// Size and up to @p maxBytes from @p offset (offset clamped to the size).
struct FileChunk
{
	uint64_t size = 0;
	uint64_t offset = 0;
	std::string bytes;
};

std::optional<FileChunk> readChunk(const fs::path& path, uint64_t offset, uint64_t maxBytes)
{
	if (path.empty()) return std::nullopt;
	auto f = forge::os::file::open(path.string(), forge::os::rd);
	if (!f) return std::nullopt;
	auto sz = f->size();
	if (!sz) return std::nullopt;
	FileChunk c;
	c.size = sz.value;
	c.offset = std::min(offset, c.size);
	const uint64_t want = std::min<uint64_t>(c.size - c.offset, maxBytes);
	c.bytes.resize(static_cast<std::size_t>(want));
	std::size_t got = 0;
	while (got < c.bytes.size())
	{
		auto r = f->pread(c.bytes.data() + got, c.bytes.size() - got, c.offset + got);
		if (!r || r.value == 0) break;
		got += r.value;
	}
	c.bytes.resize(got);
	return c;
}

/// Call @p fn for each line of the file (without the line terminator, "\r\n"
/// handled), streaming; stop when it returns false.
void forEachLine(const fs::path& path, const std::function<bool(std::string_view)>& fn)
{
	auto f = forge::os::file::open(path.string(), forge::os::rd);
	if (!f) return;
	std::string buf;
	char chunk[65536];
	auto emit = [&](std::string_view line) {
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		return fn(line);
	};
	for (;;)
	{
		auto r = f->read(chunk, sizeof chunk);
		if (!r || r.value == 0) break;
		buf.append(chunk, r.value);
		std::size_t start = 0;
		for (std::size_t nl = buf.find('\n'); nl != std::string::npos; nl = buf.find('\n', start))
		{
			if (!emit(std::string_view(buf).substr(start, nl - start))) return;
			start = nl + 1;
		}
		buf.erase(0, start);
	}
	if (!buf.empty()) (void) emit(buf);
}

// ─── LLVM pass presets ───────────────────────────────────────────────────────

std::expected<fs::path, std::string> writeLlvmPassesTempFile(const DecompilerSettings& d, bool fastPreset)
{
	auto passes = loadLlvmPassPreset(fastPreset, d.llvmPassesDisabled);
	if (!passes) return std::unexpected(passes.error());

	json::array out;
	out.reserve(passes->size());
	for (std::string& s: *passes)
		out.emplace_back(std::move(s));

	auto tmp = forge::os::fs::make_temp_file("retdec-llvm-passes-", ".json");
	if (!tmp) return std::unexpected(std::string("Could not create temporary LLVM passes file."));
	if (auto st = forge::os::write_file(tmp.value, json::dump(json::value(std::move(out)))); !st)
	{
		std::error_code ec;
		fs::remove(fs::path(tmp.value), ec);
		return std::unexpected(std::string("Could not write LLVM passes JSON."));
	}
	return fs::path(tmp.value);
}

// ─── Log → progress ──────────────────────────────────────────────────────────

struct LogStageRule
{
	const char* keyword;
	const char* stage;
	int order;
};

/// Ordered pipeline stages aligned with the progress panel's default stages.
constexpr LogStageRule kLogStageRules[] = {
	{"initialization", "Binary loading", 0},
	{"providers initialization", "Binary loading", 0},
	{"mach-o extraction", "Binary loading", 0},
	{"archive extraction", "Binary loading", 0},
	{"unpacking", "Binary loading", 0},
	{"emulation-based unpacking", "Binary loading", 0},
	{"input binary", "Binary loading", 0},
	{"decoding", "Binary loading", 0},
	{"class hierarchy", "RTTI reconstruction", 1},
	{"rtti", "RTTI reconstruction", 1},
	{"vtable", "RTTI reconstruction", 1},
	{"string", "String detection", 2},
	{"mem2reg", "SSA construction", 3},
	{"phi", "SSA construction", 3},
	{"stack", "SSA construction", 3},
	{"register", "SSA construction", 3},
	{"param", "Variable recovery", 4},
	{"return", "Variable recovery", 4},
	{"constants", "Variable recovery", 4},
	{"alias", "Alias analysis", 5},
	{"tbaa", "Alias analysis", 5},
	{"basicaa", "Alias analysis", 5},
	{"basic-aa", "Alias analysis", 5},
	{"type", "Type inference", 6},
	{"simple-types", "Type inference", 6},
	{"simple types", "Type inference", 6},
	{"cond-branch", "CFG structuring", 7},
	{"jump", "CFG structuring", 7},
	{"simplifycfg", "CFG structuring", 7},
	{"cfg", "CFG structuring", 7},
	{"syscall", "Calling convention", 8},
	{"main detection", "Calling convention", 8},
	{"unreachable", "Dead code elimination", 9},
	{"dead", "Dead code elimination", 9},
	{"dce", "Dead code elimination", 9},
	{"select-function", "Inter-procedural analysis", 10},
	{"select function", "Inter-procedural analysis", 10},
	{"inter-procedural", "Inter-procedural analysis", 10},
	{"llvm", "Inter-procedural analysis", 10},
	{"llvmir2hll", "Code generation", 11},
	{"write-ll", "Code generation", 11},
	{"write ll", "Code generation", 11},
	{"write bc", "Code generation", 11},
	{"conversion of llvm", "Code generation", 11},
	{"emitting", "Code generation", 11},
	{"hll", "Code generation", 11},
	{"library func", "STL recovery", 12},
	{"standard libr", "STL recovery", 12},
	{"stl", "STL recovery", 12},
	{"pattern", "Pattern / crypto detection", 13},
	{"crypto", "Pattern / crypto detection", 13},
	{"static code", "Pattern / crypto detection", 13},
	{"post-pipeline", "Pattern / crypto detection", 13},
};

constexpr int kPipelineStageCount = 14;

/// Remove ANSI SGR sequences (ESC [ digits/; m).
std::string stripAnsi(std::string_view line)
{
	std::string out;
	out.reserve(line.size());
	for (std::size_t i = 0; i < line.size(); ++i)
	{
		if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[')
		{
			std::size_t j = i + 2;
			while (j < line.size() && (std::isdigit(static_cast<unsigned char>(line[j])) || line[j] == ';'))
				++j;
			if (j < line.size() && line[j] == 'm')
			{
				i = j;
				continue;
			}
		}
		out.push_back(line[i]);
	}
	return out;
}

/// Drop a trailing "( 1.23s )" elapsed-time suffix.
std::string stripElapsedSuffix(std::string_view text)
{
	if (!text.empty() && text.back() == ')')
	{
		const std::size_t open = text.rfind('(');
		if (open != std::string_view::npos)
		{
			std::string_view inner = trimView(text.substr(open + 1, text.size() - open - 2));
			// [0-9.]+ \s* s
			if (!inner.empty() && inner.back() == 's')
			{
				std::string_view num = trimView(inner.substr(0, inner.size() - 1));
				const bool ok = !num.empty() && std::all_of(num.begin(), num.end(), [](char c) {
					return std::isdigit(static_cast<unsigned char>(c)) || c == '.';
				});
				if (ok) text = text.substr(0, open);
			}
		}
	}
	return trimmed(text);
}

std::string extractPhaseText(std::string_view line)
{
	const std::string clean = stripAnsi(line);
	const std::string_view t = trimView(clean);
	constexpr std::string_view kRunning = "Running phase:";
	if (const std::size_t runIdx = t.find(kRunning); runIdx != std::string_view::npos)
		return stripElapsedSuffix(trimView(t.substr(runIdx + kRunning.size())));
	if (const std::size_t subIdx = t.rfind(" -> "); subIdx != std::string_view::npos)
		return stripElapsedSuffix(trimView(t.substr(subIdx + 4)));
	return {};
}

bool matchLogStage(std::string_view phaseText, std::string* stageOut, int* orderOut)
{
	if (phaseText.empty()) return false;
	const std::string l = lower(phaseText);
	int bestOrder = -1;
	int bestKeywordLen = -1;
	const char* bestStage = nullptr;
	for (const LogStageRule& rule: kLogStageRules)
	{
		const std::string_view kw = rule.keyword;
		if (!contains(l, kw)) continue;
		const int kwLen = static_cast<int>(kw.size());
		if (kwLen > bestKeywordLen || (kwLen == bestKeywordLen && rule.order > bestOrder))
		{
			bestKeywordLen = kwLen;
			bestOrder = rule.order;
			bestStage = rule.stage;
		}
	}
	if (bestOrder < 0) return false;
	*stageOut = bestStage;
	*orderOut = bestOrder;
	return true;
}

int percentForStageOrder(int order)
{
	if (order < 0) return 0;
	return std::clamp(((order + 1) * 100) / kPipelineStageCount, 1, 99);
}

int percentFromBackendProgress(int backendPercent)
{
	// The llvmir2hll backend occupies the last ~35% of the bar.
	return std::clamp(65 + (backendPercent * 34) / 100, 1, 99);
}

std::optional<DecompileLogProgress> parseProgressLine(std::string_view line)
{
	const std::string clean = stripAnsi(line);
	const std::size_t tag = clean.find("[progress]");
	if (tag == std::string::npos) return std::nullopt;
	// [progress][^\n]*?(\d+)% : the first digit run directly followed by '%'.
	for (std::size_t k = tag + 10; k < clean.size();)
	{
		if (clean[k] == '\n') return std::nullopt;
		if (!std::isdigit(static_cast<unsigned char>(clean[k])))
		{
			++k;
			continue;
		}
		std::size_t j = k;
		while (j < clean.size() && std::isdigit(static_cast<unsigned char>(clean[j])))
			++j;
		if (j < clean.size() && clean[j] == '%')
		{
			int pct = 0;
			const auto r = std::from_chars(clean.data() + k, clean.data() + j, pct);
			if (r.ec != std::errc{}) pct = 0;
			return DecompileLogProgress{"Code generation", percentFromBackendProgress(pct)};
		}
		k = j;
	}
	return std::nullopt;
}

std::optional<DecompileLogProgress> parsePhaseLine(std::string_view line)
{
	const std::string phaseText = extractPhaseText(line);
	std::string stage;
	int order = -1;
	if (!matchLogStage(phaseText, &stage, &order)) return std::nullopt;
	return DecompileLogProgress{std::move(stage), percentForStageOrder(order)};
}

Diagnostic logDiagnostic(Severity sev, std::string_view line)
{
	Diagnostic d;
	d.severity = sev;
	d.message = std::string(line);
	d.source = "retdec-decompiler";
	return d;
}

} // namespace

// ─── TemporaryFile ───────────────────────────────────────────────────────────

TemporaryFile::~TemporaryFile()
{
	if (path_.empty()) return;
	std::error_code ec;
	fs::remove(path_, ec);
}

// ─── Arguments ───────────────────────────────────────────────────────────────

std::expected<std::vector<std::string>, std::string>
loadLlvmPassPreset(bool fastPreset, const std::vector<std::string>& disabled)
{
	const char* resource = fastPreset ? "llvm_passes_fast.json" : "llvm_passes_default.json";
	const fs::path path = resolveGuiResource(resource);
	if (path.empty()) return std::unexpected(std::format("Missing resource {}", resource));
	auto text = forge::os::read_file(path.string());
	if (!text) return std::unexpected(std::format("Missing resource {}", path.string()));

	json::value doc;
	try
	{
		doc = json::parse(text.value);
	}
	catch (const std::exception&)
	{
		return std::unexpected(std::format("Invalid LLVM passes resource {}", path.string()));
	}
	if (!doc.is_arr()) return std::unexpected(std::format("Invalid LLVM passes resource {}", path.string()));

	const std::set<std::string> off(disabled.begin(), disabled.end());
	std::vector<std::string> out;
	for (const json::value& v: doc.arr())
	{
		if (!v.is_str()) continue;
		if (!off.contains(v.str())) out.push_back(v.str());
	}
	return out;
}

std::expected<DecompilerArguments, std::string>
buildDecompilerArguments(const DecompilerLaunchRequest& req, bool writeLlvmPassesFile)
{
	DecompilerArguments out;
	std::vector<std::string>& args = out.args;
	auto add = [&args](std::initializer_list<std::string> xs) {
		for (const std::string& x: xs)
			args.push_back(x);
	};

	if (!req.decompiler.extraConfigPath.empty())
	{
		if (fileExists(req.decompiler.extraConfigPath))
			add({"--config", req.decompiler.extraConfigPath});
		else
			out.warning = std::format("Configured --config file missing: {}", req.decompiler.extraConfigPath);
	}

	add({req.binaryPath, "-o", req.outputPath, "-f", "plain"});
	if (req.silent) add({"-s"});

	const std::string outLang = trimmed(req.decompiler.outputLang);
	if (!outLang.empty()) add({"--output-lang", outLang});

	const std::string arch = trimmed(req.arch);
	if (!arch.empty()) add({"-a", arch});

	std::string profileName = trimmed(req.decompiler.decompileProfile);
	if (profileName.empty()) profileName = "balanced";
	if (req.fastDecompile) profileName = "fast";
	if (!req.decompiler.useCustomLlvmPasses) add({"--profile", profileName});

	if (req.fastDecompile)
		add({"--backend-no-opts", "--disable-static-code-detection"});
	else if (req.disableStaticCodeDetection)
		add({"--disable-static-code-detection"});
	if (req.backendNoOpts && !req.fastDecompile) add({"--backend-no-opts"});
	if (req.printAfterAll) add({"--print-after-all"});
	if (req.printBeforeAll) add({"--print-before-all"});
	if (req.emitCfg) add({"--backend-emit-cfg"});
	if (req.emitCg) add({"--backend-emit-cg"});
	if (req.keepUnreachableFuncs) add({"-k"});

	if (!req.selectedFunctions.empty()) add({"--select-functions", joinComma(req.selectedFunctions)});
	if (!req.selectedRanges.empty()) add({"--select-ranges", joinComma(req.selectedRanges)});
	if (req.selectDecodeOnly) add({"--select-decode-only"});
	if (req.entryPoint != 0) add({"--raw-entry-point", std::format("0x{:x}", req.entryPoint)});
	if (!req.pdbPath.empty() && fileExists(req.pdbPath)) add({"-p", req.pdbPath});
	if (const std::string renamer = lower(trimmed(req.varRenamer)); !renamer.empty() && renamer != "readable")
		add({"--backend-var-renamer", renamer});
	if (!req.staticCodeSigFile.empty() && fileExists(req.staticCodeSigFile))
		add({"--static-code-sigfile", req.staticCodeSigFile});
	if (req.cleanup) add({"--cleanup"});
	if (req.tryEmulation) add({"--try-emulation"});
	if (req.maxMemoryBytes > 0) add({"--max-memory", std::to_string(req.maxMemoryBytes)});
	if (req.keepLibraryFuncs) add({"--backend-keep-library-funcs"});
	if (req.keepAllBrackets) add({"--backend-keep-all-brackets"});
	if (req.noTimeVaryingInfo) add({"--backend-no-time-varying-info"});
	if (req.noVarRenaming) add({"--backend-no-var-renaming"});
	if (req.noCompoundOperators) add({"--backend-no-compound-operators"});
	if (req.noSymbolicNames) add({"--backend-no-symbolic-names"});
	if (const std::string cio = lower(trimmed(req.callInfoObtainer)); cio == "optim" || cio == "pessim")
		add({"--backend-call-info-obtainer", cio});
	const std::string arName = trimmed(req.arName);
	if (!arName.empty()) add({"--ar-name", arName});
	if (const std::string en = lower(trimmed(req.endian)); en == "little" || en == "big") add({"-e", en});
	if (req.bitSize == 16 || req.bitSize == 32 || req.bitSize == 64) add({"-b", std::to_string(req.bitSize)});
	if (req.rawSectionVma != 0) add({"--raw-section-vma", std::format("0x{:x}", req.rawSectionVma)});
	if (req.rawMode) add({"-m", "raw"});
	if (req.noMemoryLimit) add({"--no-memory-limit"});
	if (const std::string o = trimmed(req.backendDisabledOpts); !o.empty()) add({"--backend-disabled-opts", o});
	if (const std::string o = trimmed(req.backendEnabledOpts); !o.empty()) add({"--backend-enabled-opts", o});
	if (arName.empty() && req.arIndex >= 0) add({"--ar-index", std::to_string(req.arIndex)});

	if (writeLlvmPassesFile && (req.decompiler.useCustomLlvmPasses || req.fastDecompile))
	{
		auto passes = writeLlvmPassesTempFile(req.decompiler, req.fastDecompile);
		if (!passes) return std::unexpected(passes.error());
		out.llvmPassesFile = std::make_unique<TemporaryFile>(*passes);
		add({"--llvm-passes-json", passes->string()});
	}
	return out;
}

// ─── Environment ─────────────────────────────────────────────────────────────

ProcessEnvironment buildDecompilerProcessEnvironment(const AppSettings& settings, bool applyInteractiveOverrides)
{
	ProcessEnvironment env;
	if (!applyInteractiveOverrides) return env;

	if (isRegularFile(settings.ml.modelPath))
	{
		env["RETDEC_NEURAL_REFINE"] = "1";
		env["RETDEC_NEURAL_MODEL"] = absoluteClean(fs::path(settings.ml.modelPath)).string();
		if (settings.ml.contextLength >= 512) env["RETDEC_NEURAL_CTX"] = std::to_string(settings.ml.contextLength);
		if (settings.ml.maxNewTokens > 0) env["RETDEC_NEURAL_MAX_TOKENS"] = std::to_string(settings.ml.maxNewTokens);
		const bool cpuOnly = settings.ml.inferenceDevice == MLSettings::InferenceDevice::CPU;
		env["RETDEC_NEURAL_N_GPU_LAYERS"] = cpuOnly ? "0" : "-1";
		env["RETDEC_NEURAL_TEMPERATURE"] = std::format("{:.4f}", settings.ml.temperature);
		env["RETDEC_NEURAL_TOP_P"] = std::format("{:.4f}", settings.ml.topP);
		env["RETDEC_NEURAL_TOP_K"] = std::to_string(settings.ml.topK);
		if (settings.analysis.threadCount > 0) env["RETDEC_NEURAL_THREADS"] = std::to_string(settings.analysis.threadCount);
		if (const std::string sha = trimmed(settings.ml.modelSha256); !sha.empty()) env["RETDEC_NEURAL_MODEL_SHA256"] = sha;
	}
	if (!settings.cuda.useGPU) env["RETDEC_OCL_HOST"] = "0";
	if (const std::string cache = trimmed(settings.cuda.kernelCacheDir); !cache.empty())
		env["RETDEC_OCL_CACHE_DIR"] = absoluteClean(fs::path(cache)).string();
	if (settings.cuda.enableProfiling) env["RETDEC_PROFILE_JSON"] = "1";
	return env;
}

std::string processEnvironmentValue(const ProcessEnvironment& env, const std::string& name)
{
	if (const auto it = env.find(name); it != env.end()) return it->second;
	return forge::os::getenv(name, "");
}

// ─── Output paths ────────────────────────────────────────────────────────────

fs::path resolveGuiDecompiledCPath(const fs::path& binaryPath, std::string_view outputDir)
{
	// Lexical join only: making it absolute would turn "C:/bin/x.exe" into a
	// cwd-relative path on non-Windows hosts.
	const std::string fileName = binaryPath.stem().string() + ".gui-decompiled.c";
	const std::string dir = trimmed(outputDir);
	const fs::path base = dir.empty() ? binaryPath.parent_path() : fs::path(dir);
	const fs::path joined = base.empty() ? fs::path(fileName) : base / fileName;
	std::string s = joined.generic_string();
	return fs::path(s);
}

fs::path locateGuiDecompiledCPath(const fs::path& binaryPath, std::string_view outputDir)
{
	const std::string dir = trimmed(outputDir);
	std::error_code ec;
	if (!dir.empty())
	{
		const fs::path configured = resolveGuiDecompiledCPath(binaryPath, dir);
		if (fs::exists(configured, ec)) return absoluteClean(configured);
	}
	const fs::path beside = resolveGuiDecompiledCPath(binaryPath, {});
	if (fs::exists(beside, ec)) return absoluteClean(beside);
	return dir.empty() ? beside : resolveGuiDecompiledCPath(binaryPath, dir);
}

// ─── Log reading ─────────────────────────────────────────────────────────────

std::optional<DecompileLogProgress> pollDecompileLogProgress(const fs::path& logPath, uint64_t* ioFileOffset)
{
	if (!ioFileOffset || logPath.empty()) return std::nullopt;
	auto chunk = readChunk(logPath, *ioFileOffset, UINT64_MAX);
	if (!chunk) return std::nullopt;
	*ioFileOffset = chunk->offset + chunk->bytes.size();

	std::optional<DecompileLogProgress> latest;
	std::string_view text = chunk->bytes;
	std::size_t start = 0;
	while (start < text.size())
	{
		std::size_t nl = text.find('\n', start);
		if (nl == std::string_view::npos) nl = text.size();
		std::string_view line = text.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		start = nl + 1;

		std::optional<DecompileLogProgress> parsed = parseProgressLine(line);
		if (!parsed) parsed = parsePhaseLine(line);
		if (parsed && (!latest || parsed->percent >= latest->percent)) latest = std::move(parsed);
	}
	return latest;
}

DecompilerLogText readDecompilerLogForConsole(const fs::path& logPath, uint64_t maxBytes)
{
	DecompilerLogText out;
	auto sizeProbe = readChunk(logPath, 0, 0);
	if (!sizeProbe || sizeProbe->size == 0) return out;
	const uint64_t size = sizeProbe->size;
	if (size <= maxBytes)
	{
		if (auto all = readChunk(logPath, 0, size)) out.text = std::move(all->bytes);
		return out;
	}
	auto tail = readChunk(logPath, size - maxBytes, maxBytes);
	if (!tail) return out;
	std::string bytes = std::move(tail->bytes);
	if (const std::size_t nl = bytes.find('\n'); nl != std::string::npos) bytes.erase(0, nl + 1);
	out.notice = std::format("… ({} KiB log omitted, showing last {} KiB) …", size / 1024, maxBytes / 1024);
	out.text = std::move(bytes);
	return out;
}

std::string readDecompilerLogIncremental(const fs::path& logPath, uint64_t* ioFileOffset, uint64_t maxBytesPerTick)
{
	if (!ioFileOffset || logPath.empty() || maxBytesPerTick == 0) return {};
	auto chunk = readChunk(logPath, *ioFileOffset, maxBytesPerTick);
	if (!chunk) return {};
	*ioFileOffset = chunk->offset + chunk->bytes.size();
	return std::move(chunk->bytes);
}

std::string readDecompilerLogTail(const fs::path& logPath, uint64_t maxBytes)
{
	auto probe = readChunk(logPath, 0, 0);
	if (!probe) return {};
	const bool cut = probe->size > maxBytes;
	auto chunk = readChunk(logPath, cut ? probe->size - maxBytes : 0, maxBytes);
	if (!chunk) return {};
	return cut ? "…\n" + chunk->bytes : chunk->bytes;
}

std::optional<Severity> classifyDecompilerLogLine(std::string_view line)
{
	const std::string_view t = trimView(line);
	if (t.empty()) return std::nullopt;
	if (containsCI(t, "error") || startsWithCI(t, "[error]")) return Severity::Error;
	if (containsCI(t, "warning") || startsWithCI(t, "[warn]")) return Severity::Warning;
	return std::nullopt;
}

std::vector<Diagnostic> scanDecompilerLogDiagnostics(const fs::path& logPath, int maxEntries)
{
	std::vector<Diagnostic> out;
	if (logPath.empty() || maxEntries <= 0) return out;
	forEachLine(logPath, [&](std::string_view line) {
		if (const auto sev = classifyDecompilerLogLine(line)) out.push_back(logDiagnostic(*sev, trimView(line)));
		return static_cast<int>(out.size()) < maxEntries;
	});
	return out;
}

std::vector<Diagnostic>
scanDecompilerLogDiagnosticsIncremental(const fs::path& logPath, uint64_t* ioFileOffset, int maxEntries)
{
	std::vector<Diagnostic> out;
	if (!ioFileOffset || logPath.empty() || maxEntries <= 0) return out;
	auto probe = readChunk(logPath, 0, 0);
	if (!probe) return out;
	if (*ioFileOffset > probe->size) *ioFileOffset = 0; // the log was replaced: start over
	auto chunk = readChunk(logPath, *ioFileOffset, UINT64_MAX);
	if (!chunk) return out;

	const std::string_view data = chunk->bytes;
	std::size_t start = 0;
	std::size_t consumed = 0;
	while (start < data.size() && static_cast<int>(out.size()) < maxEntries)
	{
		const std::size_t nl = data.find('\n', start);
		if (nl == std::string_view::npos) break;
		const std::string_view line = data.substr(start, nl - start);
		consumed = nl + 1;
		start = nl + 1;
		if (const auto sev = classifyDecompilerLogLine(line)) out.push_back(logDiagnostic(*sev, trimView(line)));
	}
	*ioFileOffset += consumed;
	return out;
}

std::vector<Diagnostic> semanticDetectionDiagnostics(const json::value& configRoot, const AppSettings& settings, int maxEntries)
{
	std::vector<Diagnostic> out;
	if (maxEntries <= 0 || !configRoot.is_obj()) return out;
	const json::value* fns = configRoot.find("functions");
	if (!fns || !fns->is_arr()) return out;

	const RecoverySettings& rec = settings.recovery;
	const AnalysisSettings& ana = settings.analysis;
	for (const json::value& fnObj: fns->arr())
	{
		if (!fnObj.is_obj()) continue;
		const std::string fnName = fnObj.str_or("name", "");
		const json::value* dets = fnObj.find("semanticDetections");
		if (!dets || !dets->is_arr() || dets->arr().empty()) continue;

		uint64_t fnAddr = 0;
		if (const json::value* sa = fnObj.find("startAddr"); sa && sa->is_str())
			fnAddr = parseAddress(sa->str()).value_or(0);

		for (const json::value& det: dets->arr())
		{
			if (static_cast<int>(out.size()) >= maxEntries) return out;
			if (!det.is_obj()) continue;
			const std::string kind = det.str_or("kind", "");
			const std::string label = det.str_or("label", "");
			const double conf = det.num_or("confidence", 0.0);
			const std::string detail = det.str_or("detail", "");

			const std::string k = lower(kind);
			const auto has = [&k](std::initializer_list<std::string_view> words) {
				return std::any_of(words.begin(), words.end(), [&k](std::string_view w) { return contains(k, w); });
			};
			const bool isStl = has({"stl", "container"});
			const bool isCrypto = has({"crypto", "cipher", "hash"});
			const bool isPattern = has({"pattern", "idiom", "algo"});
			const bool isConcurrency = has({"concurr", "thread", "mutex"});
			const bool isCuda = has({"cuda", "opencl", "ptx"});
			const bool isType = has({"type", "typedef", "struct"});
			const bool known = conf > 0.0;

			if (!rec.detectSTL && isStl) continue;
			if (!rec.detectCrypto && isCrypto) continue;
			if (!rec.detectPatterns && isPattern) continue;
			if (!rec.detectConcurrency && isConcurrency) continue;
			if (!rec.detectCuda && isCuda) continue;
			if (!rec.detectRTTI && has({"rtti"})) continue;
			if (!rec.detectExceptions && has({"exception", "eh_"})) continue;
			if (!rec.detectVirtual && has({"virtual", "vtable"})) continue;

			if (known && conf < ana.minRecoveryConfidence) continue;
			if (known && isPattern && conf < ana.minPatternConfidence) continue;
			if (known && isType && conf < ana.minTypeConfidence) continue;
			if (!ana.enablePatternMatch && isPattern) continue;
			if (!ana.enableConcurrency && isConcurrency) continue;
			if (!ana.enableCudaRecovery && isCuda) continue;
			if (!ana.enableSerialDetect && has({"serial", "protobuf", "msgpack", "pickle"})) continue;
			if (!ana.enableModuleCluster && has({"module", "cluster", "community"})) continue;
			if (known && isStl && conf < rec.stlConfidence) continue;
			if (known && isCrypto && conf < rec.cryptoConfidence) continue;
			if (known && isPattern && conf < rec.patternConfidence) continue;
			if (known && isConcurrency && conf < rec.concurrencyConfidence) continue;

			Severity sev = Severity::Info; // Qt "Muted" (conf <= 0.5) has no Severity of its own
			if (conf > 0.8)
				sev = Severity::Info;
			else if (conf > 0.5)
				sev = Severity::Warning;

			std::string msg = label;
			if (msg.empty()) msg = kind;
			if (msg.empty()) msg = "detection";
			msg += std::format(" (confidence {:.0f}%)", conf * 100.0);
			if (!detail.empty()) msg += " — " + detail;

			Diagnostic d;
			d.severity = sev;
			d.message = std::move(msg);
			d.source = "semantic";
			d.address = fnAddr;
			d.function = fnName;
			out.push_back(std::move(d));
		}
	}
	return out;
}

} // namespace retdec::gui::model
