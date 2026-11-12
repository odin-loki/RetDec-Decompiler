/**
 * @file src/gui/model/dlg_signature_studio.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_signature_studio.h"

#include "retdec/gui/model/dlg_input.h"

#include <forge/os/os.hpp>

#include <format>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

namespace {

std::string absolutePathString(std::string_view p)
{
	std::error_code ec;
	const fs::path a = fs::absolute(fs::path(std::string(p)), ec);
	return (ec ? fs::path(std::string(p)) : a).lexically_normal().generic_string();
}

std::string parentDir(const fs::path& p)
{
	std::error_code ec;
	const fs::path a = fs::absolute(p, ec);
	return (ec ? p : a).lexically_normal().parent_path().generic_string();
}

bool existsFile(std::string_view p)
{
	if (p.empty()) return false;
	std::error_code ec;
	return fs::exists(fs::path(std::string(p)), ec);
}

} // namespace

SigStudioDefaults sigStudioDefaultsForBinary(const fs::path& binary)
{
	SigStudioDefaults d;
	if (binary.empty()) return d;
	const std::string bin = binary.generic_string();
	const std::string dir = parentDir(binary);
	d.expressLib = bin;
	d.arArchive = bin;
	d.machoFile = bin;
	d.expressOut = (fs::path(dir) / (binary.stem().string() + "-libsig.yar")).generic_string();
	d.workDir = (fs::path(dir) / "retdec-sig-work").generic_string();
	return d;
}

std::expected<SigToolCommand, std::string>
buildExpressCommand(std::string_view lib, std::string_view out, const fs::path& python, const fs::path& script)
{
	const std::string_view l = dlgTrim(lib), o = dlgTrim(out);
	if (l.empty() || o.empty()) return std::unexpected(std::string("Express: set library input and YARA output path."));
	if (python.empty() || script.empty())
		return std::unexpected(std::string(
			"Express: Python 3 or retdec-signature-from-library-creator.py not found. "
			"Use a full RetDec install with scripts/ on disk."));
	SigToolCommand c;
	c.toolLabel = "retdec-signature-from-library-creator.py";
	c.executable = python;
	c.arguments = {script.string(), "-o", std::string(o), std::string(l)};
	c.workingDirectory = parentDir(script);
	return c;
}

std::expected<SigToolCommand, std::string>
buildArExtractorCommand(const fs::path& exe, std::string_view archive, std::string_view outDir)
{
	const std::string_view a = dlgTrim(archive), o = dlgTrim(outDir);
	if (a.empty() || o.empty()) return std::unexpected(std::string("Set archive and output directory."));
	SigToolCommand c;
	c.toolLabel = "retdec-ar-extractor";
	c.executable = exe;
	c.arguments = {"-e", "-o", std::string(o), std::string(a)};
	if (!exe.empty()) c.workingDirectory = parentDir(exe);
	return c;
}

std::expected<SigToolCommand, std::string>
buildMachoExtractorCommand(const fs::path& exe, std::string_view file, std::string_view outDir)
{
	const std::string_view f = dlgTrim(file), o = dlgTrim(outDir);
	if (f.empty() || o.empty()) return std::unexpected(std::string("Set Mach-O file and output directory."));
	SigToolCommand c;
	c.toolLabel = "retdec-macho-extractor";
	c.executable = exe;
	c.arguments = {"--all", "-o", std::string(o), std::string(f)};
	if (!exe.empty()) c.workingDirectory = parentDir(exe);
	return c;
}

static std::expected<SigToolCommand, std::string> buildInOutCommand(
	std::string label, const fs::path& exe, std::string_view in, std::string_view out, std::string_view ext,
	std::string error)
{
	const std::string_view i = dlgTrim(in);
	std::string o(dlgTrim(out));
	if (i.empty()) return std::unexpected(std::move(error));
	SigToolCommand c;
	if (o.empty())
	{
		o = absolutePathString(i) + std::string(ext);
		c.defaultedOutput = o;
	}
	c.toolLabel = std::move(label);
	c.executable = exe;
	c.arguments = {"-o", o, std::string(i)};
	if (!exe.empty()) c.workingDirectory = parentDir(exe);
	return c;
}

std::expected<SigToolCommand, std::string>
buildBin2patCommand(const fs::path& exe, std::string_view in, std::string_view out)
{
	return buildInOutCommand("retdec-bin2pat", exe, in, out, ".pat", "Set bin2pat input file.");
}

std::expected<SigToolCommand, std::string>
buildPat2yaraCommand(const fs::path& exe, std::string_view in, std::string_view out)
{
	return buildInOutCommand("retdec-pat2yara", exe, in, out, ".yar", "Set pat2yara input file.");
}

std::optional<std::string> pickStaticSigFile(std::string_view expressYaraOut, std::string_view patYaraOut)
{
	std::string_view path = dlgTrim(expressYaraOut);
	if (!existsFile(path)) path = dlgTrim(patYaraOut);
	if (!existsFile(path)) return std::nullopt;
	return absolutePathString(path);
}

std::string sigToolOutputTail(std::string_view text, std::size_t maxChars)
{
	if (text.size() <= maxChars) return std::string(text);
	std::size_t at = text.size() - maxChars;
	while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) ++at;
	return std::string(text.substr(at));
}

std::string sigToolLogEntry(const SigToolResult& r)
{
	if (r.failedToStart) return "Error: " + r.outputTail;
	return std::format("--- {} exit {} ---\n{}", r.tool, r.exitCode, r.outputTail);
}

// ─── SigToolRunner ───────────────────────────────────────────────────────────

SigToolRunner::SigToolRunner() = default;
SigToolRunner::~SigToolRunner() = default;

std::expected<void, std::string> SigToolRunner::start(const SigToolCommand& cmd)
{
	if (running()) return std::unexpected(std::string("busy"));
	if (cmd.executable.empty()) return std::unexpected("Tool not found: " + cmd.toolLabel);
	auto run = std::make_unique<DecompileRun>();
	DecompileRunSpec spec;
	spec.executable = cmd.executable;
	spec.arguments = cmd.arguments;
	spec.workingDirectory = cmd.workingDirectory;
	if (auto st = run->start(std::move(spec)); !st) return std::unexpected("Failed to start: " + cmd.toolLabel);
	run_ = std::move(run);
	cmd_ = cmd;
	reported_ = false;
	return {};
}

bool SigToolRunner::running() const
{
	return run_ && run_->running();
}

std::optional<SigToolResult> SigToolRunner::collect()
{
	if (!run_ || reported_ || run_->running()) return std::nullopt;
	reported_ = true;
	SigToolResult r;
	r.tool = cmd_.toolLabel;
	r.arguments = cmd_.arguments;
	r.workingDirectory = run_->workingDirectory().generic_string();
	r.elapsedMs = run_->elapsed().count();
	if (run_->state() == DecompileRun::State::FailedToStart)
	{
		r.failedToStart = true;
		r.exitCode = -1;
		r.outputTail = run_->errorString().empty() ? "Failed to start: " + cmd_.toolLabel : run_->errorString();
		return r;
	}
	r.exitCode = run_->exitCode().value_or(-1);
	const DecompilerLogText log = readDecompilerLogForConsole(run_->logPath(), 64 * 1024);
	r.outputTail = sigToolOutputTail(log.text);
	return r;
}

std::optional<SigToolResult> SigToolRunner::poll()
{
	if (!run_) return std::nullopt;
	run_->poll();
	return collect();
}

std::optional<SigToolResult> SigToolRunner::waitFor(std::chrono::milliseconds timeout)
{
	if (!run_) return std::nullopt;
	run_->waitFor(timeout);
	return poll();
}

void SigToolRunner::cancel()
{
	if (run_) run_->cancel();
}

} // namespace retdec::gui::model
