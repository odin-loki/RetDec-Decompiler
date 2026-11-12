/**
 * @file src/gui/model/launch_options.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/launch_options.h"

#include <forge/os/os.hpp>

#include <cctype>
#include <charconv>
#include <cstring>
#include <string_view>
#include <system_error>

namespace retdec::gui::model {

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
			return false;
	return true;
}

int parseIntOrZero(const char* s)
{
	if (!s || !s[0]) return 0;
	while (*s == ' ' || *s == '\t')
		++s;
	int n = 0;
	const auto r = std::from_chars(s, s + std::strlen(s), n);
	if (r.ec != std::errc{}) return 0;
	return n;
}

} // namespace

ParsedLaunchOptions::ParsedLaunchOptions(const ParsedLaunchOptions& o):
	argStorage(o.argStorage), headless(o.headless), headlessExitMs(o.headlessExitMs),
	headlessDecompile(o.headlessDecompile), fastDecompile(o.fastDecompile), positional(o.positional),
	binaryPath(o.binaryPath)
{
	rebuildArgv();
}

ParsedLaunchOptions& ParsedLaunchOptions::operator=(const ParsedLaunchOptions& o)
{
	if (this == &o) return *this;
	argStorage = o.argStorage;
	headless = o.headless;
	headlessExitMs = o.headlessExitMs;
	headlessDecompile = o.headlessDecompile;
	fastDecompile = o.fastDecompile;
	positional = o.positional;
	binaryPath = o.binaryPath;
	rebuildArgv();
	return *this;
}

void ParsedLaunchOptions::rebuildArgv()
{
	argc = static_cast<int>(argStorage.size());
	argvPtrs.clear();
	argvPtrs.reserve(argStorage.size() + 1u);
	for (auto& s: argStorage)
		argvPtrs.push_back(s.data());
	argvPtrs.push_back(nullptr);
}

bool headlessRequestedByEnvironment()
{
	const std::string v = forge::os::getenv("RETDEC_GUI_HEADLESS", "");
	if (v.empty()) return false;
	return v == "1" || equalsIgnoreCase(v, "true") || equalsIgnoreCase(v, "yes") || equalsIgnoreCase(v, "on");
}

ParsedLaunchOptions parseLaunchOptions(int argc, char** argv)
{
	ParsedLaunchOptions out;
	out.headless = headlessRequestedByEnvironment();
	out.headlessExitMs = 0;

	if (argc > 0 && argv && argv[0]) out.argStorage.emplace_back(argv[0]);

	constexpr std::string_view kExitEq = "--headless-exit-ms=";
	for (int i = 1; i < argc && argv; ++i)
	{
		if (!argv[i]) continue;
		const std::string_view a = argv[i];
		if (a == "--headless")
		{
			out.headless = true;
			continue;
		}
		if (a == "--headless-decompile")
		{
			out.headless = true;
			out.headlessDecompile = true;
			continue;
		}
		if (a == "--fast-decompile" || a == "--fast")
		{
			out.fastDecompile = true;
			continue;
		}
		if (a == "--headless-exit-ms" && i + 1 < argc)
		{
			out.headless = true;
			out.headlessExitMs = parseIntOrZero(argv[i + 1]);
			++i;
			continue;
		}
		if (a.starts_with(kExitEq))
		{
			out.headless = true;
			out.headlessExitMs = parseIntOrZero(argv[i] + kExitEq.size());
			continue;
		}
		out.argStorage.emplace_back(a);
		if (!a.empty() && a.front() != '-') out.positional.emplace_back(a);
	}

	if (!out.positional.empty()) out.binaryPath = out.positional.front();
	out.rebuildArgv();
	return out;
}

} // namespace retdec::gui::model
