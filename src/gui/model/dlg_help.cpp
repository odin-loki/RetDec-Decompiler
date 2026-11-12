/**
 * @file src/gui/model/dlg_help.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/dlg_help.h"

#include "retdec/gui/model/cli_tool_paths.h"

#include <forge/os/fs.hpp>
#include <forge/os/os.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>

namespace retdec::gui::model {

namespace fs = std::filesystem;

std::string aboutMarkdown()
{
	return "### RetDec — Retargetable Decompiler\n"
		   "\n"
		   "A machine-code decompiler producing readable C output with RTTI, exception handling, STL, and "
		   "cryptographic primitive recovery.\n"
		   "\n"
		   "**Layout (v3):** Functions dock (left), document tabs in the centre (Decompiled C, Assembly, IR, CFG, "
		   "Synced), Workspace (Strings, Inspect, Binary) on the right, Output (Console, Problems, History, Progress) "
		   "at the bottom.\n"
		   "\n"
		   "Centre tabs: **Ctrl+1**…**Ctrl+5**. Console: **Ctrl+Backtick**; Problems: **Ctrl+Shift+Backtick**.\n"
		   "\n"
		   "**Ctrl+S** saves decompiled C; **Ctrl+Shift+S** saves the project. **F5** runs full analysis; "
		   "**Ctrl+,** opens Settings.\n"
		   "\n"
		   "File \xE2\x86\x92 Batch Decompile queues multiple binaries. Tools \xE2\x86\x92 Call Graph / Type Hierarchy / AI Assistant "
		   "open in separate windows.\n"
		   "\n"
		   "Theme: Catppuccin Latte (light), Mocha (dark), or System Default | FuseUI\n";
}

const std::vector<ShortcutGroup>& keyboardShortcutGroups()
{
	static const std::vector<ShortcutGroup> k{
		{"File",
		 {{"Ctrl+O", "Open binary"},
		  {"Ctrl+Shift+O", "Open project"},
		  {"Ctrl+S", "Save decompiled C"},
		  {"Ctrl+Shift+S", "Save project"},
		  {"Ctrl+Shift+A", "Save project as"},
		  {"Ctrl+Q", "Quit"}}},
		{"Analysis", {{"F5", "Run full analysis (decompile)"}, {"F6", "Stop analysis / batch"}}},
		{"View",
		 {{"Ctrl+1…Ctrl+5", "Centre document tabs (Decompiled C, Assembly, IR, CFG, Synced)"},
		  {"Ctrl+`", "Show Console"},
		  {"Ctrl+Shift+`", "Show Problems"},
		  {"Ctrl+G", "Go to address"},
		  {"Ctrl+,", "Settings (Tools menu)"}}},
		{"Editor / panels",
		 {{"G", "Assembly panel: go to address (when focused)"},
		  {"F", "Assembly panel: find in disassembly (when focused)"},
		  {"Ctrl+F", "Decompiled C / Synced tab: find in source"},
		  {"F3 / Shift+F3", "Decompiled C: find next / previous"},
		  {"Alt+\xE2\x86\x90 / Alt+\xE2\x86\x92", "Synced tab: navigation history"}}},
	};
	return k;
}

std::vector<fs::path> docsSearchDirs()
{
	std::vector<fs::path> out;
	auto add = [&](const fs::path& p) {
		const fs::path n = p.lexically_normal();
		if (std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
	};
	for (const fs::path& prefix: installPrefixCandidates())
	{
		add(prefix / "share" / "retdec" / "docs");
		add(prefix / "share" / "retdec" / "doc");
	}
	const fs::path bin = applicationDirPath();
	add(bin / ".." / ".." / "share" / "retdec" / "doc");
	add(bin / ".." / ".." / "docs");
	add(bin / ".." / ".." / ".." / "docs");
	const fs::path res = resolveGuiResourceDir();
	if (!res.empty())
	{
		const fs::path n = res.lexically_normal();
		if (n.filename() == "resources" && n.parent_path().filename() == "gui"
			&& n.parent_path().parent_path().filename() == "src")
			add(n.parent_path().parent_path().parent_path() / "docs");
	}
	return out;
}

static bool hasMarkdown(const fs::path& dir)
{
	std::error_code ec;
	for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
		 !ec && it != end;
		 it.increment(ec))
	{
		if (it->is_regular_file(ec) && it->path().extension() == ".md") return true;
	}
	return false;
}

fs::path findDocsDir()
{
	for (const fs::path& d: docsSearchDirs())
	{
		std::error_code ec;
		if (fs::is_directory(d, ec) && hasMarkdown(d))
		{
			const fs::path abs = fs::absolute(d, ec);
			return ec ? d : abs.lexically_normal();
		}
	}
	return {};
}

static std::string lower(std::string s)
{
	for (char& c: s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

std::vector<DocEntry> listDocs(const fs::path& dir)
{
	std::vector<DocEntry> out;
	std::error_code ec;
	if (dir.empty() || !fs::is_directory(dir, ec)) return out;
	for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
		 !ec && it != end;
		 it.increment(ec))
	{
		std::error_code e2;
		if (!it->is_regular_file(e2) || it->path().extension() != ".md") continue;
		DocEntry d;
		d.path = it->path().lexically_normal();
		d.title = it->path().lexically_relative(dir).generic_string();
		out.push_back(std::move(d));
	}
	auto rank = [](const DocEntry& d) {
		if (d.title == "README.md") return 0;
		if (d.title == "WINDOWS_NATIVE_BUILD.md") return 1;
		return 2;
	};
	std::sort(out.begin(), out.end(), [&](const DocEntry& a, const DocEntry& b) {
		const int ra = rank(a), rb = rank(b);
		if (ra != rb) return ra < rb;
		// top-level files before sub-directories, then case-insensitive
		const bool sa = a.title.find('/') != std::string::npos, sb = b.title.find('/') != std::string::npos;
		if (sa != sb) return !sa;
		return lower(a.title) < lower(b.title);
	});
	return out;
}

int defaultDocIndex(const std::vector<DocEntry>& docs)
{
	if (docs.empty()) return -1;
	for (const char* name: {"WINDOWS_NATIVE_BUILD.md", "README.md"})
		for (std::size_t i = 0; i < docs.size(); ++i)
			if (docs[i].title == name) return int(i);
	return 0;
}

std::expected<std::string, std::string> readDoc(const fs::path& path)
{
	auto r = forge::os::read_file(path.string());
	if (!r) return std::unexpected(std::format("Cannot read {}: {}", path.string(), r.err.what));
	return std::move(r.value);
}

} // namespace retdec::gui::model
