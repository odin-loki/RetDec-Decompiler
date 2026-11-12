/**
 * @file src/gui/model/cli_tool_paths.cpp
 * @brief Locate RetDec CLI tools next to retdec-gui or on PATH.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/cli_tool_paths.h"

#include <forge/os/process.hpp>

#include <mutex>
#include <string>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

namespace {

std::mutex gOverrideMutex;
fs::path gAppDirOverride;

bool existsFile(const fs::path& p)
{
	std::error_code ec;
	return !p.empty() && fs::exists(p, ec) && !fs::is_directory(p, ec);
}

bool existsDir(const fs::path& p)
{
	std::error_code ec;
	return !p.empty() && fs::is_directory(p, ec);
}

fs::path absoluteClean(const fs::path& p)
{
	std::error_code ec;
	fs::path a = fs::absolute(p, ec);
	if (ec) a = p;
	return a.lexically_normal();
}

bool isMacBundleExecutableDir(const fs::path& appDir)
{
	// .../X.app/Contents/MacOS
	const fs::path macos = appDir.filename();
	const fs::path contents = appDir.parent_path().filename();
	const fs::path bundle = appDir.parent_path().parent_path();
	return macos == "MacOS" && contents == "Contents" && bundle.extension() == ".app";
}

fs::path exeName(std::string_view base)
{
#if defined(_WIN32)
	return fs::path(std::string(base) + ".exe");
#else
	return fs::path(std::string(base));
#endif
}

fs::path findOnPath(std::string_view name)
{
	const std::string found = forge::os::find_executable(std::string(name));
	if (found.empty()) return {};
	return absoluteClean(fs::path(found));
}

fs::path firstExisting(const std::vector<fs::path>& candidates)
{
	for (const fs::path& c: candidates)
	{
		const fs::path norm = absoluteClean(c);
		if (existsFile(norm)) return norm;
	}
	return {};
}

fs::path scriptCandidateSearch(std::string_view scriptName)
{
	const fs::path appDir = applicationDirPath();
	std::vector<fs::path> candidates;
	for (const fs::path& prefix: installPrefixCandidates())
		candidates.push_back(prefix / "share" / "retdec" / "scripts" / scriptName);
	candidates.push_back(appDir / ".." / ".." / "share" / "retdec" / "scripts" / scriptName);
	candidates.push_back(appDir / ".." / "scripts" / scriptName);
	candidates.push_back(appDir / ".." / ".." / "scripts" / scriptName);
	candidates.push_back(appDir / scriptName);
	return firstExisting(candidates);
}

fs::path toolWithFallback(std::string_view primary, std::string_view fallback)
{
	fs::path p = resolveCliTool(primary);
	if (!p.empty()) return p;
	return resolveCliTool(fallback);
}

} // namespace

fs::path applicationDirPath()
{
	{
		std::lock_guard lock(gOverrideMutex);
		if (!gAppDirOverride.empty()) return gAppDirOverride;
	}
	const std::string exe = forge::os::exe_path();
	if (exe.empty()) return absoluteClean(fs::current_path());
	return absoluteClean(fs::path(exe)).parent_path();
}

void setApplicationDirPathOverride(const fs::path& dir)
{
	std::lock_guard lock(gOverrideMutex);
	gAppDirOverride = dir.empty() ? fs::path{} : absoluteClean(dir);
}

std::vector<fs::path> installPrefixCandidates()
{
	const fs::path appDir = applicationDirPath();
	std::vector<fs::path> out;
	fs::path up = (appDir / "..").lexically_normal(); // <bin>/.. ; for a bundle this is Contents/
	if (up.has_relative_path() && up.filename().empty()) up = up.parent_path(); // "x/" -> "x"
	out.push_back(up);
	// Portable layout (scripts/windows_native_build.ps1 dist\windows): the
	// executables sit flat in the prefix with share\ beside them, not in bin\.
	out.push_back(appDir);
	if (isMacBundleExecutableDir(appDir))
	{
		// X.app/Contents/MacOS -> install prefix holding X.app, bin/ and share/.
		out.push_back(appDir.parent_path().parent_path().parent_path());
	}
	return out;
}

fs::path resolveCliTool(std::string_view toolBaseName)
{
	if (toolBaseName.empty()) return {};
	const fs::path name = exeName(toolBaseName);
	const fs::path appDir = applicationDirPath();

	const fs::path sideBySide = appDir / name;
	if (existsFile(sideBySide)) return absoluteClean(sideBySide);

	if (isMacBundleExecutableDir(appDir))
	{
		const fs::path prefixBin = appDir.parent_path().parent_path().parent_path() / "bin" / name;
		if (existsFile(prefixBin)) return absoluteClean(prefixBin);
	}

	return findOnPath(name.string());
}

fs::path resolveRetdecFileinfoExecutable()
{
	return toolWithFallback("retdec-fileinfo", "fileinfo");
}

fs::path resolveRetdecUnpackerExecutable()
{
	return toolWithFallback("retdec-unpacker", "unpacker");
}

fs::path resolveRetdecDecompilerExecutablePath()
{
	return resolveCliTool("retdec-decompiler");
}

fs::path resolveBundledDecompilerConfigPath()
{
	const fs::path dec = resolveRetdecDecompilerExecutablePath();
	if (dec.empty()) return {};
	const fs::path bin = dec.parent_path();
	return firstExisting({
		bin / ".." / "share" / "retdec" / "decompiler-config.json",
		bin / ".." / ".." / "share" / "retdec" / "decompiler-config.json",
	});
}

fs::path resolveRetdecArExtractorExecutable()
{
	return toolWithFallback("retdec-ar-extractor", "ar_extractor");
}

fs::path resolveRetdecMachoExtractorExecutable()
{
	return resolveCliTool("retdec-macho-extractor");
}

fs::path resolveRetdecBin2patExecutable()
{
	return resolveCliTool("retdec-bin2pat");
}

fs::path resolveRetdecPat2yaraExecutable()
{
	return resolveCliTool("retdec-pat2yara");
}

fs::path resolvePythonInterpreter()
{
	fs::path p = findOnPath("python3");
	if (!p.empty()) return p;
	p = findOnPath("python");
	if (!p.empty()) return p;
#if defined(_WIN32)
	p = findOnPath("py.exe");
	if (!p.empty()) return p;
#endif
	return {};
}

fs::path resolveSignatureFromLibraryCreatorScript()
{
	return scriptCandidateSearch("retdec-signature-from-library-creator.py");
}

fs::path resolveRetdecExportIntelScript()
{
	return scriptCandidateSearch("retdec_export_intel.py");
}

namespace {

std::vector<fs::path> guiResourceDirCandidates()
{
	std::vector<fs::path> out;
	for (const fs::path& prefix: installPrefixCandidates())
		out.push_back((prefix / "share" / "retdec" / "gui").lexically_normal());
#ifdef RETDEC_GUI_SOURCE_RESOURCE_DIR
	out.push_back(fs::path(RETDEC_GUI_SOURCE_RESOURCE_DIR).lexically_normal());
#endif
	return out;
}

} // namespace

fs::path resolveGuiResourceDir()
{
	for (const fs::path& d: guiResourceDirCandidates())
		if (existsDir(d)) return absoluteClean(d);
	return {};
}

fs::path resolveGuiResource(std::string_view name)
{
	if (name.empty()) return {};
	for (const fs::path& d: guiResourceDirCandidates())
	{
		const fs::path p = d / name;
		if (existsFile(p)) return absoluteClean(p);
	}
	return {};
}

} // namespace retdec::gui::model
