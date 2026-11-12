// FUSE UI core: UiContext, node storage, widget classes, events, focus, layout, paint, a11y. Plan §4.4–§4.7, UI2.
//
// One UiContext owns one tree and is bound to one thread (its owner): every call except post() must come from it.
// Other threads post UiCommands through a bounded lock-free MPSC queue drained at the start of update().
//
// Frame (UiContext::update):  commands → timers → animations → style (dirty nodes) → layout (dirty subtrees, measure
// cache, fixed-size subtrees are boundaries) → paint (draw list rebuilt when anything paint-dirty; damage rect = old ∪
// new bounds of changed nodes) → a11y version bump. A frame with nothing dirty does no work and allocates nothing.
#pragma once

#include "fuse/text/text.hpp"
#include "fuse/ui/draw_list.hpp"
#include "fuse/ui/handle.hpp"
#include "fuse/ui/inline_fn.hpp"
#include "fuse/ui/style.hpp"
#include "fuse/ui/types.hpp"

#include <atomic>
#include <concepts>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui {

class UiContext;

// ---- Input ---------------------------------------------------------------------------------------------------------
/// UI key set (maps 1:1 onto fuse::platform key set v2 names; see platform/events_v2.hpp).
enum class Key : u16 {
    None, Tab, Enter, Escape, Space, Backspace, Delete, Left, Right, Up, Down, Home, End, PageUp, PageDown,
    A, C, V, X, Y, Z, F2,
    W, S, D, Q, E, N, O, F5, F10, ///< appended for the editor shell (fly camera, File / Play shortcuts)
    Num1, Num2, Num3, Num4, ///< editor transform tools (Select / Move / Rotate / Scale)
    B, F, G, H, I, J, K, L, M, P, R, T, U, Num0, Num5, Num6, Num7, Num8, Num9, ///< rebindable editor shortcuts
    F1, F3, F4, F6, F7, F8, F9, F11, F12,
    BracketLeft, BracketRight, ///< timeline previous / next key
    Other
};
enum Mods : u8 { kModShift = 1, kModCtrl = 2, kModAlt = 4, kModSuper = 8 };
enum class MouseButton : u8 { Left, Right, Middle };
enum class NavDir : u8 { Left, Right, Up, Down };
enum class FocusReason : u8 { Pointer, Keyboard, Programmatic };

enum class EventType : u8 {
    PointerMove, PointerDown, PointerUp, PointerEnter, PointerLeave, Click, Wheel, KeyDown, KeyUp, Text, FocusIn,
    FocusOut,
    Composition, ///< IME pre-edit changed: `text` = pre-edit (empty ends it), `caret` = byte offset in it
};
enum class Phase : u8 { Capture, Target, Bubble };

struct Event {
    EventType type = EventType::PointerMove;
    Vec2 pos;              ///< pointer events: context coordinates
    MouseButton button = MouseButton::Left;
    Key key = Key::None;
    u8 mods = 0;
    Vec2 wheel;            ///< wheel notches (y > 0 = up)
    std::string_view text; ///< Text: UTF-8 (valid during dispatch only)
    FocusReason focusReason = FocusReason::Programmatic;
    i32 caret = 0;         ///< Composition: caret byte offset into `text`
};

/// Clipboard backend of a context (UI1). Default: a context-local buffer; apps install the platform clipboard
/// (fuse::platform::setClipboardText / clipboardText, see platform_bridge.hpp).
struct ClipboardHooks {
    bool (*set)(void* user, std::string_view utf8) = nullptr;
    bool (*get)(void* user, std::string& out) = nullptr;
    void* user = nullptr;
};

// ---- Signals -------------------------------------------------------------------------------------------------------
enum class SignalKind : u8 { Clicked, Toggled, ValueChanged, TextChanged, Submitted, FocusChanged, HoverChanged, Event };
struct SignalArgs {
    f32 value = 0.f;
    bool flag = false;
    std::string_view text{};
    const Event* event = nullptr;
};
using Slot = InlineFn<void(UiContext&, NodeHandle, const SignalArgs&)>;
using CommandFn = void (*)(UiContext&, NodeHandle, u64 a, u64 b);

// ---- Accessibility -------------------------------------------------------------------------------------------------
enum class A11yRole : u8 {
    None, Window, Group, Text, Button, CheckBox, RadioButton, Switch, Slider, ProgressBar, TextField, ScrollArea,
    List, ListItem, Image, Separator, Link, ComboBox, SpinBox, Dial, ScrollBar, Tab
};
const char* a11yRoleName(A11yRole r);
/// One accessible node in a flattened snapshot (UiContext::snapshotA11y). Nodes with role None are skipped and
/// their children re-parented, as in dumpA11y. `parent` indexes the snapshot vector (~0u for the top node).
struct A11ySnapshotNode {
    NodeHandle node;
    u32 parent = ~0u;
    A11yRole role = A11yRole::None;
    std::string name; ///< a11y name, else the node text
    Rect bounds;      ///< window coordinates (logical px)
    bool focusable = false, focused = false, checked = false, disabled = false;
    f32 value = 0.f, lo = 0.f, hi = 0.f;
};

// ---- Widget classes ------------------------------------------------------------------------------------------------
struct PaintContext {
    UiContext& ui;
    NodeHandle node;
    Rect rect;    ///< border box
    Rect content; ///< inside padding
    const ComputedStyle& style;
    u32 states;
    f32 opacity;  ///< accumulated (multiply colours by it)
    Color tint(Color c) const { return c.withAlpha(c.a * opacity); }
};

struct EventContext {
    UiContext& ui;
    NodeHandle node; ///< node whose handler runs
    NodeHandle target;
    Phase phase;
};

/// A registered table of functions (no inheritance, no moc). Null entries use core defaults.
struct WidgetClass {
    std::string name;
    u32 nameHash = 0;
    A11yRole role = A11yRole::Group;
    bool focusable = false;
    bool interactive = false; ///< receives pointer press / click (buttons, fields)
    /// Content size (excluding padding) for `available` content width (may be +inf). Null: text or zero.
    Size (*measure)(UiContext&, NodeHandle, Size available) = nullptr;
    /// Paints after the core background / border; null: paints the node's text.
    void (*paint)(const PaintContext&, DrawList&) = nullptr;
    /// Returns true when consumed (stops propagation).
    bool (*onEvent)(EventContext&, const Event&) = nullptr;
    /// Default style (applied before style sheets).
    ComputedStyle defaults{};
};

/// Concept turning a plain struct with static members into a WidgetClass.
template <class W>
concept WidgetType = requires {
    { W::kName } -> std::convertible_to<std::string_view>;
};

class WidgetRegistry {
public:
    u32 add(WidgetClass cls); ///< returns class id (re-registering a name replaces it)
    template <WidgetType W>
    u32 registerWidget() {
        WidgetClass c;
        c.name = std::string(W::kName);
        if constexpr (requires { W::kRole; }) c.role = W::kRole;
        if constexpr (requires { W::kFocusable; }) c.focusable = W::kFocusable;
        if constexpr (requires { W::kInteractive; }) c.interactive = W::kInteractive;
        if constexpr (requires { &W::measure; }) c.measure = &W::measure;
        if constexpr (requires { &W::paint; }) c.paint = &W::paint;
        if constexpr (requires { &W::onEvent; }) c.onEvent = &W::onEvent;
        if constexpr (requires { W::defaults(); }) c.defaults = W::defaults();
        return add(std::move(c));
    }
    u32 find(std::string_view name) const; ///< 0xFFFFFFFF when unknown
    const WidgetClass& get(u32 id) const { return m_classes[id]; }
    u32 size() const { return u32(m_classes.size()); }

private:
    std::vector<WidgetClass> m_classes;
};

// ---- Context -------------------------------------------------------------------------------------------------------
struct UiContextDesc {
    Size viewport{800.f, 600.f};
    const fuse::text::Font* font = nullptr; ///< non-owning; null = no text (measure 0)
    u32 reserveNodes = 1024;
    int atlasSize = 1024;
};

enum class UiError : u8 { InvalidHandle, UnknownClass, QueueFull, StyleParse };

struct FrameStats {
    u32 styled = 0, measured = 0, arranged = 0, painted = 0, drawCmds = 0;
    bool repainted = false;
};

class UiContext {
public:
    explicit UiContext(const UiContextDesc& desc = {});
    ~UiContext();
    UiContext(const UiContext&) = delete;
    UiContext& operator=(const UiContext&) = delete;

    WidgetRegistry& registry() { return m_registry; }
    const WidgetRegistry& registry() const { return m_registry; }
    StyleEngine& styles() { return m_styles; }
    std::expected<void, UiError> addStyleSheet(std::string_view css, std::vector<StyleDiagnostic>* diags = nullptr);
    void addStyleSheet(StyleSheet sheet); ///< an already parsed (or cooked) sheet
    void clearStyleSheets();

    // tree
    NodeHandle root() const { return m_root; }
    std::expected<NodeHandle, UiError> create(std::string_view className, NodeHandle parent, std::string_view id = {});
    NodeHandle create(u32 classId, NodeHandle parent, std::string_view id = {});
    void destroy(NodeHandle n);
    bool alive(NodeHandle n) const;
    void moveTo(NodeHandle n, NodeHandle newParent, NodeHandle before = {});
    NodeHandle parent(NodeHandle n) const;
    NodeHandle firstChild(NodeHandle n) const;
    NodeHandle nextSibling(NodeHandle n) const;
    u32 childCount(NodeHandle n) const;
    u32 nodeCount() const { return m_live; }
    NodeHandle find(std::string_view id) const;
    NodeHandle handleOf(u32 index) const;
    std::string_view idOf(NodeHandle n) const;
    u32 classOf(NodeHandle n) const;

    // properties
    void addClass(NodeHandle n, std::string_view cls);
    void removeClass(NodeHandle n, std::string_view cls);
    bool hasClass(NodeHandle n, std::string_view cls) const;
    void set(NodeHandle n, Prop p, const PropValue& v);
    bool setCss(NodeHandle n, std::string_view property, std::string_view value);
    void clearInline(NodeHandle n);
    void setText(NodeHandle n, std::string_view s);
    std::string_view text(NodeHandle n) const;
    void setValue(NodeHandle n, f32 v, bool emit = false);
    f32 value(NodeHandle n) const;
    void setRange(NodeHandle n, f32 lo, f32 hi, f32 step = 0.f);
    f32 rangeMin(NodeHandle n) const;
    f32 rangeMax(NodeHandle n) const;
    f32 rangeStep(NodeHandle n) const;
    void setState(NodeHandle n, u32 stateBits, bool on);
    u32 states(NodeHandle n) const;
    void setChecked(NodeHandle n, bool on) { setState(n, kStateChecked, on); }
    bool checked(NodeHandle n) const { return states(n) & kStateChecked; }
    void setDisabled(NodeHandle n, bool on) { setState(n, kStateDisabled, on); }
    bool disabled(NodeHandle n) const { return states(n) & kStateDisabled; }
    void setFocusable(NodeHandle n, bool on);
    void setTabIndex(NodeHandle n, i32 index);
    void setFocusScope(NodeHandle n, bool on);
    void setA11y(NodeHandle n, A11yRole role, std::string_view name = {});
    A11yRole role(NodeHandle n) const;
    void setUserData(NodeHandle n, u64 v);
    u64 userData(NodeHandle n) const;
    /// Per-node text edit buffer (created on first use; LineEdit and friends).
    fuse::text::TextEditBuffer& editBuffer(NodeHandle n);
    const fuse::text::TextLayout* textLayout(NodeHandle n) const;

    // style / geometry results (valid after update)
    const ComputedStyle& computed(NodeHandle n) const;
    Rect rect(NodeHandle n) const;
    Rect contentRect(NodeHandle n) const;
    Vec2 scroll(NodeHandle n) const;
    Size scrollExtent(NodeHandle n) const;
    void scrollTo(NodeHandle n, Vec2 offset);
    static constexpr f32 kScrollBarSize = 8.f; ///< overlay scrollbar thickness (drawn and draggable on scroll containers)
    bool scrollDragging() const { return m_scrollDrag.node.valid(); }

    // frame
    void setViewport(Size s);
    Size viewport() const { return m_viewport; }
    void update(f32 dtSeconds = 0.f);
    bool needsUpdate() const; ///< false when an on-demand app can sleep
    /// Seconds until the context needs an update: 0 when dirty / animating / commands pending, the earliest timer
    /// otherwise, +inf when idle (an on-demand loop sleeps this long or until input).
    f32 nextWakeSeconds() const;
    const DrawList& drawList() const { return m_draw; }
    /// OS-window popups (Qt: a QMenu / combo list is its own top-level window). When on, overlay popup panels (the
    /// "clamp" nodes popupMenuAt / combo boxes / submenus create) are left out of drawList(); each is painted into its
    /// own list in popup-local coordinates (popupSurfaces()) for the host to show in a borderless OS window at
    /// window origin + rect, so a popup may extend past the window. They then clamp to `screen` (the work area in
    /// context coordinates; empty = the viewport) instead of the viewport. Pointer input for a popup window is fed
    /// back in context coordinates (local + rect origin).
    struct PopupSurface {
        NodeHandle node;
        Rect rect;      ///< context coordinates (may lie outside the viewport)
        DrawList draw;  ///< origin = rect top-left
        u64 revision = 0; ///< bumps whenever `draw` or `rect` changes
    };
    void setOsPopups(bool on, Rect screen = {});
    bool osPopups() const { return m_osPopups; }
    /// Where popups may be placed: the screen bounds with OS popups, the viewport otherwise.
    Rect popupArea() const;
    const std::vector<PopupSurface>& popupSurfaces() const { return m_popupSurfaces; }
    Rect damage() const { return m_damage; }
    const FrameStats& stats() const { return m_stats; }
    u64 frameIndex() const { return m_frame; }
    void markAllDirty();
    /// Repaints `n` next update (widgets whose paint reads external state, e.g. item models).
    void invalidate(NodeHandle n) { if (node(n)) markPaint(n.index); }

    // input (returns true when the UI consumed it — game input arbitration)
    bool pointerMove(Vec2 p);
    bool pointerDown(Vec2 p, MouseButton b = MouseButton::Left);
    bool pointerUp(Vec2 p, MouseButton b = MouseButton::Left);
    bool wheel(Vec2 p, Vec2 notches);
    /// Modifier state (kMod*) stamped on pointer events; platform layers update it on every modifier change.
    void setModifiers(u8 mods) { m_mods = mods; }
    u8 modifiers() const { return m_mods; }
    bool keyDown(Key k, u8 mods = 0);
    bool keyUp(Key k, u8 mods = 0);
    bool textInput(std::string_view utf8);
    /// IME pre-edit for the focused node (empty `utf8` ends it); `caret` is a byte offset into it. The pre-edit is
    /// dropped when focus moves.
    bool textComposition(std::string_view utf8, i32 caret);
    std::string_view composition() const { return m_composition; }
    i32 compositionCaret() const { return m_compositionCaret; }
    /// Text-input session: true while the focused node edits text (a widget reported a caret this frame via
    /// setTextCaret). The platform glue opens the IME / virtual keyboard and places the candidate window at
    /// textCaret().
    bool wantsTextInput() const { return node(m_focus) && m_caretNode == m_focus; }
    Rect textCaret() const { return m_caret; }
    void setTextCaret(NodeHandle n, Rect caret) { m_caretNode = n; m_caret = caret; }
    // clipboard
    void setClipboard(const ClipboardHooks& hooks) { m_clipboard = hooks; }
    bool setClipboardText(std::string_view utf8);
    bool clipboardText(std::string& out) const;
    NodeHandle hitTest(Vec2 p) const;
    NodeHandle hovered() const { return m_hover; }
    Vec2 pointer() const { return m_pointer; } ///< last pointer position
    NodeHandle pressed() const { return m_pressed; }
    NodeHandle focused() const { return m_focus; }
    bool focusVisible() const { return m_focusVisible; }
    void setFocus(NodeHandle n, FocusReason why = FocusReason::Programmatic);
    bool focusNext(bool backward = false);
    bool navigate(NavDir d);
    void setModal(NodeHandle scope); ///< restricts focus and pointer input to `scope` (invalid handle clears)
    void setSpatialNavigation(bool on) { m_spatialNav = on; }
    /// Tab order of the current focus scope (tests, inspector).
    void tabOrder(std::vector<NodeHandle>& out) const;

    // signals
    ConnectionHandle connect(NodeHandle n, SignalKind kind, Slot slot);
    void disconnect(ConnectionHandle c);
    void emit(NodeHandle n, SignalKind kind, const SignalArgs& args);

    // timers and animation (seconds in the context's time domain)
    TimerHandle startTimer(f32 seconds, bool repeat, InlineFn<void(UiContext&)> fn);
    void cancelTimer(TimerHandle t);
    enum class AnimProp : u8 { Opacity, OffsetX, OffsetY, Value };
    enum class Ease : u8 { Linear, InOutCubic, OutBack };
    void animate(NodeHandle n, AnimProp p, f32 to, f32 seconds, Ease e = Ease::InOutCubic);
    bool animating() const { return m_animCount > 0; }

    // cross-thread commands (any thread)
    bool post(CommandFn fn, NodeHandle n, u64 a = 0, u64 b = 0);

    // dumps (tests, inspector)
    void dumpLayout(std::string& out) const;
    void dumpA11y(std::string& out) const;
    /// Flattened accessible tree of the visible nodes, root first, parents before children (for platform
    /// accessibility adapters: fuse_ui_a11y / AccessKit). Clears `out` first.
    void snapshotA11y(std::vector<A11ySnapshotNode>& out) const;

    // text
    const fuse::text::Font* font() const { return m_font; }
    fuse::text::GlyphAtlas& atlas() { return m_atlas; }
    /// Emits glyph commands for a laid-out block at `origin` (top-left).
    void paintTextLayout(DrawList& dl, const fuse::text::TextLayout& tl, Vec2 origin, Color c, f32 pixelSize, u32 node);
    /// Lays out and paints a one-off string (widgets' secondary labels). Uses an internal scratch layout.
    void paintString(DrawList& dl, std::string_view s, Rect box, Color c, f32 pixelSize, TextAlignment align, u32 node);
    f32 measureString(std::string_view s, f32 pixelSize);

    struct Node;
    struct Cold;

private:
    friend struct ContextSubject;
    Node* node(NodeHandle h);
    const Node* node(NodeHandle h) const;
    u32 alloc();
    void unlink(u32 i);
    void linkBefore(u32 i, u32 parent, u32 before);
    void destroyRec(u32 i);
    void markLayout(u32 i);
    void markStyle(u32 i);
    void markPaint(u32 i);
    void stylePass();
    void styleRec(u32 i, bool parentChanged);
    Size measure(u32 i, Size avail);
    void arrange(u32 i, Rect box);
    void arrangeChildren(u32 i);
    void paintPass();
    void paintRec(u32 i, f32 opacity, DrawList& dl);
    /// Overlay scrollbar geometry of a scroll container along an axis (0 = x, 1 = y); false when it does not overflow.
    bool scrollBarGeom(u32 i, u8 axis, Rect& track, Rect& thumb) const;
    NodeHandle hitRec(u32 i, Vec2 p) const;
    bool dispatch(u32 target, const Event& e);
    void setHover(u32 i);
    u32 interactiveAncestor(u32 i) const;
    u32 scopeOf(u32 i) const;
    bool inScope(u32 i, u32 scope) const;
    bool focusableNode(u32 i) const;
    void drainCommands();
    void ensureText(u32 i, f32 width);
    void collectTab(std::vector<u32>& out) const;
    void markArrange(u32 i);
    bool visibleChain(u32 i) const;

    WidgetRegistry m_registry;
    StyleEngine m_styles;
    const fuse::text::Font* m_font = nullptr;
    fuse::text::GlyphAtlas m_atlas;
    std::vector<Node> m_nodes;
    std::vector<std::unique_ptr<Cold>> m_cold;
    std::vector<u32> m_free;
    u32 m_live = 0;
    NodeHandle m_root;
    Size m_viewport;
    u32 m_layerClass = 0xFFFFFFFFu;
    DrawList m_draw;
    Rect m_damage;
    bool m_osPopups = false;
    Rect m_popupScreen;
    std::vector<u32> m_popupPending;
    std::vector<PopupSurface> m_popupSurfaces;
    u64 m_popupRevision = 0;
    FrameStats m_stats;
    u64 m_frame = 0;
    bool m_anyStyleDirty = true, m_anyLayoutDirty = true, m_anyPaintDirty = true;
    u32 m_seenSheetVersion = 0;
    NodeHandle m_hover, m_pressed, m_focus, m_modal;
    bool m_focusVisible = false;
    bool m_spatialNav = false;
    std::string m_composition;
    i32 m_compositionCaret = 0;
    NodeHandle m_caretNode;
    Rect m_caret;
    ClipboardHooks m_clipboard;
    std::string m_localClipboard;
    Vec2 m_pointer{-1.f, -1.f};
    u8 m_mods = 0;
    struct ScrollDrag { NodeHandle node; u8 axis = 0; f32 grab = 0.f; } m_scrollDrag;
    std::vector<u32> m_path;
    std::vector<u32> m_scratchOrder;
    std::vector<std::pair<u32, i32>> m_scratchTab;
    std::vector<fuse::text::ShapedGlyph> m_shapeScratch;
    fuse::text::TextLayout m_stringScratch;

    struct Connection {
        u32 node = 0xFFFFFFFFu, nodeGen = 0, gen = 0;
        SignalKind kind = SignalKind::Clicked;
        Slot slot;
    };
    std::vector<Connection> m_connections;
    std::vector<u32> m_freeConnections;

    struct Timer {
        f32 interval = 0.f, left = 0.f;
        bool repeat = false, active = false;
        u32 gen = 0;
        InlineFn<void(UiContext&)> fn;
    };
    std::vector<Timer> m_timers;

    struct Anim {
        NodeHandle node;
        AnimProp prop = AnimProp::Opacity;
        Ease ease = Ease::Linear;
        f32 from = 0.f, to = 0.f, t = 0.f, duration = 0.f;
        bool active = false;
    };
    std::vector<Anim> m_anims;
    u32 m_animCount = 0;

    struct Command {
        std::atomic<u64> seq{0};
        CommandFn fn = nullptr;
        NodeHandle node;
        u64 a = 0, b = 0;
    };
    static constexpr u32 kQueueSize = 256;
    std::unique_ptr<Command[]> m_queue;
    std::atomic<u64> m_head{0};
    u64 m_tail = 0;
};

/// Registers the core classes "panel" (generic box), "label" (text) and "root".
void registerCoreClasses(WidgetRegistry& r);

} // namespace fuse::ui
