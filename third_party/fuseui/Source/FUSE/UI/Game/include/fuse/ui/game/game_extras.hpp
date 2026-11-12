// FUSE UI7 game UI, second part (plan §4.15): inventory grids with drag and drop, touch controls (virtual sticks and
// buttons), loading / transition screens, rich dialogue (markup tags, pauses, voice-line sync, events) and the UI
// sound theme. Like game_ui.hpp these are plain retained UI on fuse_ui_widgets; engine bridges (ECS UiSurface,
// fuse_audio) live in ecs_surface.hpp / ui_audio.hpp and are built in tree only.
#pragma once

#include "fuse/ui/game/game_ui.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::game {

/// Registers the extra widget classes (invslot, touchstick, touchbutton, spinner, richtext) and their theme.
/// Called by install(); safe to call again.
void installExtras(UiContext& ui);

// ---- inventory ---------------------------------------------------------------------------------------------------
struct ItemDef {
    std::string name;
    Color color = Color::rgb(0x8899AA);
    u32 maxStack = 1;
    u32 kind = 0; ///< bit index tested against a grid's accept mask
};
struct ItemStack {
    u32 item = 0; ///< 0 = empty
    u32 count = 0;
    bool empty() const { return item == 0 || count == 0; }
};
/// Item definitions by id (1-based; 0 is "empty").
class ItemCatalog {
public:
    u32 add(ItemDef d) { m_defs.push_back(std::move(d)); return u32(m_defs.size()); }
    const ItemDef* get(u32 id) const { return id == 0 || id > m_defs.size() ? nullptr : &m_defs[id - 1]; }

private:
    std::vector<ItemDef> m_defs;
};

/// Inventory grids (containers) of slots with drag and drop between them: pointer drag (press, move, release over a
/// slot) and gamepad / keyboard pick-and-place (activate() on the focused slot picks, activate() again places).
/// Placing on an empty slot moves, on the same stackable item merges (the remainder goes back), on another item swaps
/// when both containers accept the kinds; releasing anywhere else returns the stack. A tooltip follows the hovered or
/// focused slot. The drag ghost and tooltip live in `layer` (an anchor / absolute container covering the screen).
class Inventory {
public:
    static constexpr f32 kSlot = 52.f;
    Inventory(UiContext& ui, const ItemCatalog& items, NodeHandle layer);
    /// Adds a container of cols x rows focusable slots under `parent`; accepts items whose kind bit is in `acceptMask`.
    u32 addGrid(NodeHandle parent, u32 cols, u32 rows, std::string_view id, u32 acceptMask = ~0u);
    u32 grids() const { return u32(m_grids.size()); }
    u32 slotCount(u32 grid) const { return u32(m_grids[grid].slots.size()); }
    ItemStack get(u32 grid, u32 slot) const { return m_grids[grid].slots[slot].stack; }
    void set(u32 grid, u32 slot, ItemStack s);
    NodeHandle slotNode(u32 grid, u32 slot) const { return m_grids[grid].slots[slot].node; }
    NodeHandle gridNode(u32 grid) const { return m_grids[grid].node; }
    u32 total(u32 item) const; ///< count of `item` across all grids (and the hand)

    // pointer drag; each returns true when the inventory consumed the event
    bool pointerDown(Vec2 p);
    bool pointerMove(Vec2 p);
    bool pointerUp(Vec2 p);
    // gamepad / keyboard: picks the focused slot's stack, or places the held stack on it
    bool activate();
    void cancel(); ///< returns a held stack to its origin
    bool holding() const { return !m_hand.empty(); }
    ItemStack hand() const { return m_hand; }
    /// Moves the tooltip to the hovered / focused slot and the ghost to the pointer; call once per frame.
    void update();
    NodeHandle tooltip() const { return m_tip; }
    NodeHandle ghost() const { return m_ghost; }
    u32 drops() const { return m_drops; }
    u32 rejects() const { return m_rejects; }

private:
    struct Slot { NodeHandle node; ItemStack stack; };
    struct Grid { NodeHandle node; u32 accept = ~0u; std::vector<Slot> slots; };
    bool find(NodeHandle n, u32& g, u32& s) const;
    bool accepts(u32 g, ItemStack s) const;
    void refresh(u32 g, u32 s);
    void pick(u32 g, u32 s);
    bool place(u32 g, u32 s);
    void returnHand();

    UiContext& m_ui;
    const ItemCatalog& m_items;
    NodeHandle m_layer, m_ghost, m_tip;
    std::vector<Grid> m_grids;
    ItemStack m_hand;
    u32 m_fromGrid = 0, m_fromSlot = 0;
    bool m_pointerDrag = false, m_pressArmed = false;
    Vec2 m_pressAt, m_pointer;
    u32 m_drops = 0, m_rejects = 0;
};

// ---- touch controls ----------------------------------------------------------------------------------------------
/// Multi-touch virtual sticks and buttons. A stick claims a touch that starts in its zone; floating sticks re-centre
/// on the touch. stick() is the knob offset over the radius (length <= 1, dead zone applied, +y down). Buttons are
/// circles; a touch that starts on one holds it until released.
class TouchControls {
public:
    TouchControls(UiContext& ui, NodeHandle layer);
    u32 addStick(Rect zone, f32 radius = 56.f, bool floating = true, f32 deadZone = 0.15f);
    u32 addButton(Vec2 center, f32 radius, std::string_view label);
    void touchDown(u32 id, Vec2 p);
    void touchMove(u32 id, Vec2 p);
    void touchUp(u32 id);
    Vec2 stick(u32 s) const;
    bool held(u32 b) const { return m_buttons[b].touch != kNone; }
    bool pressed(u32 b) const { return m_buttons[b].edges & 1u; }  ///< went down since the last endFrame()
    bool released(u32 b) const { return m_buttons[b].edges & 2u; } ///< went up since the last endFrame()
    void endFrame();
    /// Shown only while touch is the active device (GameUi::noteDevice -> setVisible).
    void setVisible(bool on);
    bool visible() const { return m_visible; }
    NodeHandle stickNode(u32 s) const { return m_sticks[s].node; }
    NodeHandle buttonNode(u32 b) const { return m_buttons[b].node; }

private:
    static constexpr u32 kNone = 0xFFFFFFFFu;
    struct Stick { NodeHandle node; Rect zone; f32 radius, dead; bool floating; u32 touch = kNone; Vec2 base, knob; };
    struct Button { NodeHandle node; Vec2 c; f32 r; u32 touch = kNone; u32 edges = 0; };
    void place(Stick& s);
    UiContext& m_ui;
    NodeHandle m_layer;
    std::vector<Stick> m_sticks;
    std::vector<Button> m_buttons;
    bool m_visible = true;
};

// ---- loading / transition screens --------------------------------------------------------------------------------
/// Full-screen loading screen that keeps animating (spinner, tips, progress easing) in real time while the world
/// streams in and the game clock is paused. show() fades in, finish() waits for the bar to reach 100 %, then fades out
/// and hides. Transitions (fadeTo) reuse the same plate without the loading furniture.
class LoadingScreen {
public:
    LoadingScreen(UiContext& ui, NodeHandle layer);
    void show(std::string_view title, std::string_view tips = {}, f32 fadeSeconds = 0.25f);
    void setProgress(f32 p01);
    void finish(f32 fadeSeconds = 0.4f);
    /// Plain fade to black and back (scene transitions): opaque after `inSeconds`, then fades out.
    void fadeTo(f32 inSeconds, f32 holdSeconds, f32 outSeconds);
    void update(f32 realDt);
    bool visible() const { return m_phase != Phase::Hidden; }
    bool opaque() const { return m_alpha >= 1.f; }
    f32 alpha() const { return m_alpha; }
    f32 shownProgress() const { return m_shown; }
    f32 spinnerAngle() const { return m_angle; }
    u32 tipIndex() const { return m_tip; }
    NodeHandle node() const { return m_root; }

private:
    enum class Phase : u8 { Hidden, In, Shown, Out, FadeIn, FadeHold };
    void apply();
    UiContext& m_ui;
    NodeHandle m_root, m_title, m_bar, m_spin, m_tipLabel, m_pct;
    Phase m_phase = Phase::Hidden;
    f32 m_alpha = 0.f, m_fadeIn = 0.25f, m_fadeOut = 0.4f, m_hold = 0.f;
    f32 m_target = 0.f, m_shown = 0.f, m_angle = 0.f, m_tipTime = 0.f;
    bool m_finishing = false, m_plain = false;
    std::vector<std::string> m_tips;
    u32 m_tip = 0;
};

// ---- rich dialogue -----------------------------------------------------------------------------------------------
/// Markup: [b]bold[/b], [i]italic[/i], [color=#RRGGBB]..[/color] (or a name: red, gold, blue, green, grey),
/// [shake]..[/shake], [pause=0.5] (seconds), [t=1.25] (voice-line time of the next character), [event=name]
/// (fires when revealed), [[ for a literal '['. Errors report the byte offset.
struct RichSpan {
    u32 begin = 0, end = 0; ///< byte range in `plain`
    Color color = Color::rgb(0xE6E8EE);
    bool bold = false, italic = false, shake = false;
};
struct RichText {
    std::string plain;
    std::vector<RichSpan> spans;                        ///< cover `plain` in order
    std::vector<std::pair<u32, f32>> pauses;            ///< byte offset -> seconds
    std::vector<std::pair<u32, f32>> cues;              ///< byte offset -> voice time (ascending)
    std::vector<std::pair<u32, std::string>> events;    ///< byte offset -> name
};
std::expected<RichText, std::string> parseRich(std::string_view markup);

/// Dialogue box with a speaker plate and a rich-text body revealed by a typewriter in game time. With a voice line,
/// reveal follows the voice clock: [t=] cues pin characters to times and text between cues is spread evenly; without
/// cues the whole line spans the clip. Events fire through the hook when their character is revealed.
class RichDialogue {
public:
    using EventHook = void (*)(void* user, std::string_view name);
    RichDialogue(UiContext& ui, NodeHandle parent);
    ~RichDialogue();
    bool say(std::string_view speaker, std::string_view markup, f32 charsPerSecond = 40.f);
    bool sayVoiced(std::string_view speaker, std::string_view markup, f32 voiceSeconds);
    /// Advances the reveal. `voiceTime` >= 0 overrides the internal voice clock (pass the audio playback position).
    void update(f32 gameDt, f32 voiceTime = -1.f);
    void skip(); ///< completes the line, then closes the box
    bool open() const { return m_open; }
    bool complete() const { return m_shown >= m_text.plain.size(); }
    u32 visibleBytes() const { return u32(m_shown); }
    std::string_view visible() const { return std::string_view(m_text.plain).substr(0, m_shown); }
    const RichText& text() const { return m_text; }
    std::string_view lastError() const { return m_error; }
    void setEventHook(EventHook fn, void* user) { m_hook = fn; m_hookUser = user; }
    NodeHandle node() const { return m_box; }
    NodeHandle body() const { return m_body; }

private:
    bool start(std::string_view speaker, std::string_view markup);
    void reveal(std::size_t upto);
    UiContext& m_ui;
    NodeHandle m_box, m_speaker, m_body;
    RichText m_text;
    std::string m_error;
    std::size_t m_shown = 0, m_nextEvent = 0;
    f32 m_cps = 40.f, m_acc = 0.f, m_pause = 0.f, m_voice = -1.f, m_voiceLen = 0.f, m_time = 0.f;
    bool m_open = false;
    EventHook m_hook = nullptr;
    void* m_hookUser = nullptr;
};

// ---- UI sound theme ----------------------------------------------------------------------------------------------
/// `@sounds { move: ui/move.wav; accept: ui/accept.wav; back: ..; error: ..; open: ..; close: ..; volume: 0.8; }`
/// inside a theme sheet. extractSoundTheme removes the block (the CSS engine never sees it) and fills `out`.
struct SoundTheme {
    std::string clips[6]; ///< by UiSound
    f32 volume = 1.f;
    std::string_view clip(UiSound s) const { return clips[u32(s)]; }
};
std::string extractSoundTheme(std::string_view css, SoundTheme& out, std::vector<std::string>* errors = nullptr);
std::string_view soundName(UiSound s);

} // namespace fuse::ui::game
