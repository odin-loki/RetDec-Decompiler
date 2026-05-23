/**
 * @file include/retdec/gui/launch_options.h
 * @brief Headless / CI launch options for retdec-gui (offscreen platform, argv filtering).
 *
 * Environment:
 *   RETDEC_GUI_HEADLESS=1|true|yes — if QT_QPA_PLATFORM is unset, force offscreen (no windows).
 *
 * Arguments (stripped before QApplication sees them):
 *   --headless              — same as above
 *   --headless-exit-ms N    — quit the event loop after N ms (smoke / automated debug)
 *   --headless-exit-ms=N
 *   --headless-decompile    — after opening a binary, run retdec-decompiler and quit
 *                             when the subprocess finishes (parity / CI timing)
 *   --fast-decompile        — with --headless-decompile, use the Fast preset
 */

#ifndef RETDEC_GUI_LAUNCH_OPTIONS_H
#define RETDEC_GUI_LAUNCH_OPTIONS_H

#include <string>
#include <vector>

namespace retdec::gui {

struct ParsedLaunchOptions {
    std::vector<std::string> argStorage;
    std::vector<char*>       argvPtrs; ///< argvPtrs.size()==argStorage.size()+1, last is nullptr
    int                      argc = 0;
    bool                     headless = false;
    /// >0: QTimer::singleShot to quit (smoke test). 0: run until window closed.
    int headlessExitMs = 0;
    /// Run full decompile on the opened binary, then quit (ignores headlessExitMs).
    bool headlessDecompile = false;
    bool fastDecompile     = false;
};

/**
 * Read env and argv; may set QT_QPA_PLATFORM=offscreen. Build filtered argv for QApplication.
 */
ParsedLaunchOptions parseLaunchOptions(int argc, char** argv);

} // namespace retdec::gui

#endif
