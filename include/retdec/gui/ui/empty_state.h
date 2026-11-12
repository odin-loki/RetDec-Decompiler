/**
 * @file include/retdec/gui/ui/empty_state.h
 * @brief EmptyState (centred icon + title + hint shown by panels with no
 *        content yet) and the small panel scaffolding every view of this
 *        directory shares: the panel root node that routes keyboard shortcuts
 *        (capture and bubble phase) to its owner, show / hide helpers and
 *        shortcut matching.
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#ifndef RETDEC_GUI_UI_EMPTY_STATE_H
#define RETDEC_GUI_UI_EMPTY_STATE_H

#include <fuse/ui/ui.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace retdec::gui::ui {

/// Centred icon + title + hint for panels with no content yet.
class EmptyState {
public:
	EmptyState(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);
	EmptyState(const EmptyState&) = delete;
	EmptyState& operator=(const EmptyState&) = delete;

	fuse::ui::NodeHandle root() const { return root_; }

	void setTitle(std::string_view title);
	/// An empty hint hides the hint line.
	void setHint(std::string_view hint);
	/// A glyph (e.g. "⌂") shown large above the title; empty hides it.
	void setIcon(std::string_view glyph);

	std::string title() const;
	std::string hint() const;
	std::string icon() const;
	bool hintShown() const { return hintShown_; }
	bool iconShown() const { return iconShown_; }

private:
	fuse::ui::UiContext& ui_;
	fuse::ui::NodeHandle root_, icon_, title_, hint_;
	bool hintShown_ = false;
	bool iconShown_ = false;
};

// ── Panel scaffolding ────────────────────────────────────────────────────────

/// Receives every event that passes through a panel root (capture phase on
/// the way down, bubble phase on the way up). Return true to consume.
struct PanelEventHandler
{
	std::function<bool(fuse::ui::EventContext&, const fuse::ui::Event&)> handle;
};

/// Creates a panel root ("retdec-panel": a flex column filling its parent)
/// whose events go to @p handler (owned by the panel; must outlive the node or
/// be detached with releasePanelRoot).
fuse::ui::NodeHandle createPanelRoot(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent, PanelEventHandler* handler,
                                     std::string_view cssClass = {});

/// Detaches the handler and destroys the panel's subtree (panel destructors).
void releasePanelRoot(fuse::ui::UiContext& ui, fuse::ui::NodeHandle root);

/// True for nodes that take typed text (line edits, spin boxes, code editors):
/// plain-letter shortcuts are not taken from them.
bool isTextEntry(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle n);

/// display: flex / none.
void setShown(fuse::ui::UiContext& ui, fuse::ui::NodeHandle n, bool shown);

/// True when @p e is a KeyDown of @p key with exactly @p mods (shift / ctrl / alt).
bool isKey(const fuse::ui::Event& e, fuse::ui::Key key, fuse::ui::u8 mods = 0);

/// A horizontal tool row (class "retdec-toolbar").
fuse::ui::NodeHandle toolbarRow(fuse::ui::UiContext& ui, fuse::ui::NodeHandle parent);

/// Sets a line edit's text (buffer and node text) without emitting TextChanged.
void setLineEditText(fuse::ui::UiContext& ui, fuse::ui::NodeHandle edit, std::string_view text);

/// Registers the classes and default styles of this directory (idempotent;
/// every panel constructor calls it). Also registers FuseUI's bundled faces
/// (Open Sans, JetBrains Mono for "monospace", DejaVu Sans glyph fallback)
/// when the context has no monospace face yet (AppWindow already has them).
void ensureRetdecUiClasses(fuse::ui::UiContext& ui);

/// CSS classes of the shared sheet: "retdec-mono" (code, addresses, hex: the
/// monospace family) and "retdec-bold".
inline constexpr std::string_view kMonoClass = "retdec-mono";
inline constexpr std::string_view kBoldClass = "retdec-bold";
/// The CSS family list used for code, addresses and hex.
inline constexpr std::string_view kMonoFamily = "\"JetBrains Mono\", monospace";

/// The face for text a cell delegate / custom widget paints in @p node with
/// paintString: the node's own family (or the monospace one when @p mono) at
/// bold weight when @p bold. Null when that is the node's own face (pass it
/// straight to paintString).
const fuse::text::Font* styledFont(const fuse::ui::UiContext& ui, fuse::ui::NodeHandle node, bool mono, bool bold);

} // namespace retdec::gui::ui

#endif // RETDEC_GUI_UI_EMPTY_STATE_H
