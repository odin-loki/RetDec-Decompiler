/**
 * @file src/gui/model/dlg_target.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_target.h"

#include "retdec/gui/model/dlg_input.h"

#include <forge/json/json.hpp>
#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <algorithm>
#include <exception>
#include <format>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

TargetFields targetFieldsFromProject(const ProjectFile& pf)
{
	return {pf.arch(), pf.os(), dlgFormatEntryPoint(pf.entryPoint())};
}

std::expected<void, std::string> applyTargetFields(ProjectFile& pf, const TargetFields& f)
{
	pf.setArch(std::string(dlgTrim(f.arch)));
	pf.setOs(std::string(dlgTrim(f.os)));
	const auto ep = dlgParseEntryPoint(f.entry);
	if (!ep) return std::unexpected(ep.error());
	pf.setEntryPoint(*ep);
	return {};
}

std::expected<void, std::string> setDecompilerConfigPath(AppSettings& s, const fs::path& path)
{
	if (path.empty())
		s.decompiler.extraConfigPath.clear();
	else
	{
		std::error_code ec;
		const fs::path abs = fs::absolute(path, ec);
		s.decompiler.extraConfigPath = (ec ? path : abs).lexically_normal().generic_string();
	}
	auto saved = s.save();
	s.notifySettingsChanged();
	return saved;
}

bool decompilerConfigEditable(std::string_view path)
{
	const std::string_view p = dlgTrim(path);
	if (p.empty()) return false;
	std::error_code ec;
	return fs::is_regular_file(fs::path(std::string(p)), ec);
}

std::string targetConfigBrowseStart(std::string_view configuredPath, std::string_view binaryPath)
{
	const std::string_view cfg = dlgTrim(configuredPath);
	if (!cfg.empty()) return std::string(cfg);
	if (!binaryPath.empty() && binaryPath != "—")
	{
		std::error_code ec;
		const fs::path abs = fs::absolute(fs::path(std::string(binaryPath)), ec);
		return (ec ? fs::path(std::string(binaryPath)) : abs).lexically_normal().parent_path().generic_string();
	}
	return {};
}

std::expected<void, std::string> validateJsonText(std::string_view text)
{
	try
	{
		(void)forge::json::parse(text);
	}
	catch (const std::exception& e)
	{
		std::string msg = e.what();
		if (msg.rfind("json: ", 0) == 0) msg.erase(0, 6);
		return std::unexpected(std::move(msg));
	}
	return {};
}

std::expected<std::string, std::string> readTextFile(const fs::path& path)
{
	auto r = forge::os::read_file(path.string());
	if (!r) return std::unexpected(std::format("Cannot open:\n{}", path.string()));
	return std::move(r.value);
}

std::expected<void, std::string> writeTextFile(const fs::path& path, std::string_view text)
{
	auto st = forge::os::write_file(path.string(), text);
	if (!st) return std::unexpected(std::format("Cannot write {}: {}", path.string(), st.err.what));
	return {};
}

int batchAddPaths(std::vector<std::string>& list, const std::vector<std::string>& added)
{
	int n = 0;
	for (const std::string& p: added)
	{
		if (dlgTrim(p).empty()) continue;
		std::error_code ec;
		const fs::path abs = fs::absolute(fs::path(p), ec);
		const std::string s = (ec ? fs::path(p) : abs).lexically_normal().generic_string();
		if (std::find(list.begin(), list.end(), s) != list.end()) continue;
		list.push_back(s);
		++n;
	}
	return n;
}

void batchRemoveIndices(std::vector<std::string>& list, std::vector<int> indices)
{
	std::sort(indices.begin(), indices.end());
	indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
	for (auto it = indices.rbegin(); it != indices.rend(); ++it)
		if (*it >= 0 && std::size_t(*it) < list.size()) list.erase(list.begin() + *it);
}

} // namespace retdec::gui::model
