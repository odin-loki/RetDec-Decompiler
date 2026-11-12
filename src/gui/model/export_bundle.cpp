/**
 * @file src/gui/model/export_bundle.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/export_bundle.h"

#include "retdec/gui/model/artifact_loader.h"
#include "retdec/gui/model/zip_writer.h"

#include <forge/os/os.hpp>

#include <algorithm>
#include <format>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

namespace {

bool isFile(const fs::path& p)
{
	std::error_code ec;
	return !p.empty() && fs::is_regular_file(p, ec);
}

std::expected<void, std::string> addFileIfExists(ZipWriter& zip, const fs::path& diskPath)
{
	if (!isFile(diskPath)) return {};
	return zip.addFile(diskPath, diskPath.filename().string());
}

fs::path withSuffix(const fs::path& p, std::string_view suffix)
{
	return fs::path(p.string() + std::string(suffix));
}

} // namespace

std::string formatDecompilerCommandText(const DecompileBundleInput& in)
{
	std::string line = std::format("executable: {}\n", in.decompilerExe);
	if (!in.decompilerCwd.empty()) line += std::format("cwd: {}\n", in.decompilerCwd);
	line += "command:";
	for (const std::string& a: in.decompilerArgs)
		line += ' ' + a;
	line += '\n';
	return line;
}

std::expected<void, std::string> exportDecompileBundle(const DecompileBundleInput& in, const fs::path& zipPath)
{
	if (!isFile(in.cPath)) return std::unexpected(std::format("Decompiled C file not found: {}", in.cPath.string()));

	const DecompileArtifactPaths paths = pathsFromOutputC(in.cPath);

	ZipWriter zip(zipPath);
	if (!zip.isOpen()) return std::unexpected(zip.errorString());

	for (const fs::path& p: {
			 paths.cPath,
			 paths.configPath,
			 paths.dsmPath,
			 paths.llPath,
			 withSuffix(in.cPath, ".refined.c"),
			 withSuffix(in.cPath, ".refinement-manifest.json"),
			 withSuffix(in.cPath, ".type-inference.json"),
		 })
	{
		if (auto r = addFileIfExists(zip, p); !r) return r;
	}

	// CFG / call-graph dumps: <stem>.cfg.*.dot and <stem>.cg.dot, sorted by name.
	{
		const fs::path dir = fs::absolute(in.cPath).parent_path();
		const std::string stem = in.cPath.stem().string(); // e.g. sample.gui-decompiled
		std::vector<std::string> dots;
		std::error_code ec;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec)) continue;
			const std::string name = it->path().filename().string();
			const std::string cfgPrefix = stem + ".cfg.";
			const bool cfgDot =
				name.size() >= cfgPrefix.size() + 4 && name.starts_with(cfgPrefix) && name.ends_with(".dot");
			if (cfgDot || name == stem + ".cg.dot") dots.push_back(name);
		}
		std::sort(dots.begin(), dots.end());
		for (const std::string& name: dots)
			if (auto r = addFileIfExists(zip, dir / name); !r) return r;
	}

	if (auto r = zip.addEntry("decompiler-command.txt", formatDecompilerCommandText(in)); !r) return r;

	std::string logBytes;
	if (!in.logFilePath.empty())
		if (auto r = forge::os::read_file(in.logFilePath.string())) logBytes = std::move(r.value);
	if (logBytes.empty() && !in.logInline.empty()) logBytes = in.logInline;
	if (!logBytes.empty())
		if (auto r = zip.addEntry("decompiler.log", logBytes); !r) return r;

	return zip.close();
}

} // namespace retdec::gui::model
