/**
 * @file include/retdec/gui/model/dlg_help.h
 * @brief Help menu content: the About text, the Keyboard Shortcuts table and
 *        locating / reading the Markdown documentation for Help →
 *        Documentation.
 *
 * Documentation directories, in order (the Qt GUI looked in the first four
 * relative to the binary; the FuseUI viewer renders every *.md it finds there):
 *   1. `<prefix>/share/retdec/docs` and `<prefix>/share/retdec/doc` for each
 *      installPrefixCandidates() entry (`<bin>/..` first);
 *   2. `<bin>/../../share/retdec/doc`, `<bin>/../../docs`, `<bin>/../../../docs`
 *      (build trees);
 *   3. the source tree's `docs/` when the GUI resources resolve to the source
 *      tree (`src/gui/resources`).
 *
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_MODEL_DLG_HELP_H
#define RETDEC_GUI_MODEL_DLG_HELP_H

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace retdec::gui::model {

/// Help → About RetDec (Markdown; the Qt dialog's HTML, same words).
std::string aboutMarkdown();
inline constexpr std::string_view kAboutTitle = "About RetDec";

/// One row of the Keyboard Shortcuts dialog.
struct ShortcutEntry
{
	std::string keys;
	std::string action;
};
struct ShortcutGroup
{
	std::string title; ///< "File", "Analysis", "View", "Editor / panels"
	std::vector<ShortcutEntry> entries;
};
/// The Qt dialog's table, group by group.
const std::vector<ShortcutGroup>& keyboardShortcutGroups();

/// Candidate documentation directories (existing or not), in search order.
std::vector<std::filesystem::path> docsSearchDirs();
/// The first candidate directory that exists and holds at least one *.md.
std::filesystem::path findDocsDir();

/// A Markdown file of the docs directory.
struct DocEntry
{
	std::string title;            ///< path relative to the docs dir, '/' separated ("gui/FUSEUI_PORT.md")
	std::filesystem::path path;   ///< absolute
};
/// Every *.md under @p dir (recursive), README.md first, the Qt viewer's
/// WINDOWS_NATIVE_BUILD.md next, then the rest sorted case-insensitively.
std::vector<DocEntry> listDocs(const std::filesystem::path& dir);
/// The entry Help → Documentation opens first (the Qt preference order:
/// WINDOWS_NATIVE_BUILD.md, README.md), else 0. -1 for an empty list.
int defaultDocIndex(const std::vector<DocEntry>& docs);
/// File contents (UTF-8).
std::expected<std::string, std::string> readDoc(const std::filesystem::path& path);
/// Shown when no documentation directory exists.
inline constexpr std::string_view kNoDocsMessage = "Could not find documentation under share/retdec/doc/ or docs/.";

} // namespace retdec::gui::model

#endif // RETDEC_GUI_MODEL_DLG_HELP_H
