/**
 * @file include/retdec/gui/model/launch_options.h
 * @brief retdec-gui command line and headless options (no UI toolkit involved).
 *
 * Environment:
 *   RETDEC_GUI_HEADLESS=1|true|yes|on — run without an OS window.
 *
 * Arguments (stripped from the filtered argv):
 *   --headless              — same as above
 *   --headless-exit-ms N    — quit after N ms (smoke / automated debug)
 *   --headless-exit-ms=N
 *   --headless-decompile    — after opening the binary, run retdec-decompiler
 *                             exactly as the CLI would, load the artefacts and
 *                             exit with the decompiler's status (implies headless)
 *   --fast / --fast-decompile — with --headless-decompile, use the Fast preset
 *
 * Every other argument is kept, in order, in the filtered argv (argStorage /
 * argvPtrs / argc) that is handed to the toolkit. The first kept argument that
 * does not start with '-' is the binary (or .retdec project) to open.
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_LAUNCH_OPTIONS_H
#define RETDEC_GUI_MODEL_LAUNCH_OPTIONS_H

#include <optional>
#include <string>
#include <vector>

namespace retdec::gui::model {

struct ParsedLaunchOptions
{
	std::vector<std::string> argStorage; ///< argv[0] plus every argument not consumed above
	std::vector<char*> argvPtrs;         ///< argvPtrs.size()==argStorage.size()+1, last is nullptr
	int argc = 0;
	bool headless = false;
	/// >0: quit after this many ms (smoke test). 0: run until the window closes.
	int headlessExitMs = 0;
	/// Run a full decompile of the opened binary, then quit (ignores headlessExitMs).
	bool headlessDecompile = false;
	bool fastDecompile = false;
	/// Kept arguments that do not start with '-', in order (argv[0] excluded).
	std::vector<std::string> positional;
	/// positional.front(), when there is one.
	std::optional<std::string> binaryPath;

	ParsedLaunchOptions() = default;
	// argvPtrs points into argStorage: copies re-point it.
	ParsedLaunchOptions(const ParsedLaunchOptions& o);
	ParsedLaunchOptions& operator=(const ParsedLaunchOptions& o);
	ParsedLaunchOptions(ParsedLaunchOptions&&) noexcept = default;
	ParsedLaunchOptions& operator=(ParsedLaunchOptions&&) noexcept = default;

	/// Rebuild argvPtrs / argc from argStorage.
	void rebuildArgv();
};

/// True when RETDEC_GUI_HEADLESS is 1/true/yes/on (case-insensitive).
bool headlessRequestedByEnvironment();

/// Read the environment and @p argv; build the filtered argv. Never modifies
/// the process environment.
ParsedLaunchOptions parseLaunchOptions(int argc, char** argv);

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_LAUNCH_OPTIONS_H
