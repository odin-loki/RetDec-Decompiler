// FUSE UI7 — game UI (plan §4.15): HUD widgets, world markers, world-space panels, gamepad navigation, input
// arbitration, device prompts, safe area, time domains, UI sounds, locale tables, subtitles and dialogue.
//
// Everything here is plain retained UI on top of fuse_ui / fuse_ui_widgets: HUD elements are widget classes whose
// paint reads node state (value / text / userData), and GameUi is a per-context controller that advances the
// game-time elements (bar trails, floating text, kill feed, subtitles, typewriter) from a GameClock so pausing the
// game freezes them while menus keep animating in real time. No threads; pools are sized up front so a steady
// HUD allocates nothing per frame.
#pragma once

#include "fuse/ui/ui.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::game {

struct Vec3 {
    f32 x = 0.f, y = 0.f, z = 0.f;
};
/// Column-major 4x4 (OpenGL / Vulkan convention, clip = M * [x y z 1]).
using Mat4 = std::array<f32, 16>;
Mat4 perspective(f32 fovYRadians, f32 aspect, f32 zNear, f32 zFar);
Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up);
Mat4 mul(const Mat4& a, const Mat4& b);

/// Registers the game widget classes (hudbar, cooldown, compass, radialmenu, worldmarker, prompt, floattext) and the
/// HUD theme. Call after widgets::install.
void install(UiContext& ui);
std::string_view hudThemeCss();

// ---- time domains ----------------------------------------------------------------------------------------------
enum class TimeDomain : u8 { Real, Game };
struct GameClock {
    f32 scale = 1.f;
    bool paused = false;
    double realTime = 0.0, gameTime = 0.0;
    /// Advances both clocks; returns the game-domain delta (0 while paused).
    f32 tick(f32 realDt) {
        realTime += realDt;
        const f32 g = paused ? 0.f : realDt * scale;
        gameTime += g;
        return g;
    }
    f32 delta(TimeDomain d, f32 realDt) const { return d == TimeDomain::Real ? realDt : (paused ? 0.f : realDt * scale); }
};

// ---- HUD widgets -----------------------------------------------------------------------------------------------
/// Health / resource bar with a damage trail (the trail eases down to the value in game time, GameUi::update).
NodeHandle hudBar(UiContext& ui, NodeHandle parent, f32 value01, std::string_view id = {});
void setHudBar(UiContext& ui, NodeHandle bar, f32 value01);
f32 hudBarTrail(const UiContext& ui, NodeHandle bar);
/// Cooldown dial: a clock-wipe over an icon label; `remaining01` 1 = just used, 0 = ready.
NodeHandle cooldownDial(UiContext& ui, NodeHandle parent, std::string_view label, std::string_view id = {});
void setCooldown(UiContext& ui, NodeHandle dial, f32 remaining01);
/// Compass strip: heading in degrees (0 = north, clockwise); one objective bearing (NaN = none).
NodeHandle compass(UiContext& ui, NodeHandle parent, std::string_view id = {});
void setCompass(UiContext& ui, NodeHandle c, f32 headingDeg, f32 objectiveBearingDeg);
/// Radial menu: items separated by '\n'. radialSelect picks the slice under a stick / pointer direction
/// (dead zone 0.35); radialAccept emits Submitted with value = index.
NodeHandle radialMenu(UiContext& ui, NodeHandle parent, std::string_view items, std::string_view id = {});
int radialSelect(UiContext& ui, NodeHandle menu, Vec2 dir);
int radialSelected(const UiContext& ui, NodeHandle menu);
bool radialAccept(UiContext& ui, NodeHandle menu);

// ---- input devices, prompts, gamepad navigation, arbitration ----------------------------------------------------
enum class InputDevice : u8 { KeyboardMouse, Xbox, PlayStation, Touch };
enum class Action : u8 { Accept, Back, Menu, Interact, Count };
/// Glyph text for an action on a device ("Enter", "A", "Cross", "Tap").
std::string_view promptGlyph(InputDevice d, Action a);

struct GamepadState {
    Vec2 stick;               ///< left stick, +y down
    bool up = false, down = false, left = false, right = false;
    bool a = false, b = false, start = false;
};
/// Gamepad -> UI: d-pad / stick drive spatial navigate() with an initial delay then repeat, A activates the focused
/// widget (Enter), B is Back (Escape). Runs in real time (menus work while the game is paused).
struct GamepadNavConfig {
    f32 deadZone = 0.5f;
    f32 repeatDelay = 0.4f;
    f32 repeatInterval = 0.12f;
};
class GamepadNav {
public:
    using Config = GamepadNavConfig;
    explicit GamepadNav(UiContext& ui, Config cfg = Config{}) : m_ui(ui), m_cfg(cfg) {}
    /// Returns the number of navigation moves made this call (repeat included).
    int update(const GamepadState& s, f32 realDt);
    u32 accepts() const { return m_accepts; }
    u32 backs() const { return m_backs; }

private:
    UiContext& m_ui;
    Config m_cfg;
    int m_dir = -1;
    f32 m_held = 0.f, m_nextRepeat = 0.f;
    bool m_a = false, m_b = false;
    u32 m_accepts = 0, m_backs = 0;
};

enum class InputOwner : u8 { Game, Ui };
/// Decides per device who gets input: the UI owns the pointer over interactive HUD / menu nodes and while it holds
/// a press; the keyboard while a text field is focused or a menu is open; the gamepad while a menu is open.
class InputArbiter {
public:
    explicit InputArbiter(UiContext& ui) : m_ui(ui) {}
    void setMenuOpen(bool on) { m_menu = on; }
    bool menuOpen() const { return m_menu; }
    InputOwner pointerOwner(Vec2 p) const;
    InputOwner keyboardOwner() const;
    InputOwner gamepadOwner() const { return m_menu ? InputOwner::Ui : InputOwner::Game; }
    /// Routes a pointer event: forwards to the UI when it owns it, returns the owner (Game = the game handles it).
    InputOwner pointerMove(Vec2 p);
    InputOwner pointerDown(Vec2 p);
    InputOwner pointerUp(Vec2 p);
    InputOwner key(Key k, bool down);

private:
    UiContext& m_ui;
    bool m_menu = false;
    bool m_uiPress = false;
};

// ---- locale ----------------------------------------------------------------------------------------------------
/// Key -> string tables per locale ("key=value" lines). Bound nodes are re-texted when the locale switches.
class StringTable {
public:
    void add(std::string_view locale, std::string_view kvLines);
    bool setLocale(std::string_view locale);
    std::string_view locale() const { return m_locale; }
    std::string_view tr(std::string_view key) const; ///< falls back to the key
    void bind(UiContext& ui, NodeHandle n, std::string_view key);
    void refresh(UiContext& ui);

private:
    struct Entry { std::string locale, key, value; };
    std::vector<Entry> m_entries;
    std::vector<std::pair<NodeHandle, std::string>> m_bound;
    std::string m_locale;
};

// ---- sounds ----------------------------------------------------------------------------------------------------
enum class UiSound : u8 { Move, Accept, Back, Error, Open, Close };
using SoundHook = void (*)(void* user, UiSound s);

// ---- world-space UI panels -------------------------------------------------------------------------------------
/// A UI context rendered to an image and placed in the world as a rectangle (centre, unit right / up axes, size in
/// world units). Pointer input arrives as rays and is converted to panel pixels.
struct UiSurface {
    UiContext* ui = nullptr;
    Vec3 center, right{1, 0, 0}, up{0, 1, 0};
    f32 width = 1.f, height = 1.f;
    bool depthTest = true;
    Image texture;
    u64 renders = 0;
};
/// Ray / panel intersection: panel pixel coordinates (top-left origin) and ray distance; nullopt on miss / back-face.
std::optional<Vec2> surfaceRaycast(const UiSurface& s, Vec3 origin, Vec3 dir, f32* t = nullptr);
/// Feeds a ray pointer to the surface's context; returns true when the ray hit the panel.
bool surfacePointer(UiSurface& s, Vec3 origin, Vec3 dir, bool down, bool up);
/// Updates the context and re-rasterises the texture when something changed; true when the texture changed.
bool surfaceRender(UiSurface& s, f32 dt);

// ---- controller ------------------------------------------------------------------------------------------------
struct Insets {
    f32 left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
};

class GameUi {
public:
    static constexpr u32 kFloatPool = 24;
    static constexpr u32 kFeedLines = 5;
    static constexpr u32 kMarkerPool = 16;

    /// Builds the HUD layer (absolute, full viewport, pointer-transparent) under `parent` (default: root).
    explicit GameUi(UiContext& ui, NodeHandle parent = {});
    UiContext& ui() { return m_ui; }
    NodeHandle hud() const { return m_hud; }
    NodeHandle safeArea() const { return m_safe; }
    GameClock& clock() { return m_clock; }

    /// Advances real- and game-time elements; call before UiContext::update.
    void update(f32 realDt);

    void setSafeArea(Insets in);
    Insets safeAreaInsets() const { return m_insets; }

    // floating combat text (pooled, rises and fades in game time)
    void floatText(Vec2 screen, std::string_view text, Color c, f32 seconds = 1.f);
    u32 floatTextActive() const;
    // kill feed (newest at the bottom, lines expire in game time)
    NodeHandle killFeed() const { return m_feed; }
    void feed(std::string_view text, f32 seconds = 4.f);
    u32 feedLines() const;
    // subtitles (real time: they keep timing with audio, which the game pauses separately)
    void subtitle(std::string_view speaker, std::string_view text, f32 seconds);
    std::string_view subtitleText() const;
    // typewriter dialogue (game time); skip() completes the line, then closes it
    void say(std::string_view speaker, std::string_view text, f32 charsPerSecond = 40.f);
    void skip();
    bool dialogueOpen() const { return m_dlgOpen; }
    bool dialogueComplete() const { return m_dlgShown >= m_dlgText.size(); }
    std::string_view dialogueVisible() const { return std::string_view(m_dlgText).substr(0, m_dlgShown); }

    // world markers: projected with the camera, clamped to the safe area with an edge arrow when off-screen or
    // behind, and faded by distance between fadeNear and fadeFar
    u32 addMarker(Vec3 world, std::string_view label);
    void moveMarker(u32 id, Vec3 world);
    void removeMarker(u32 id);
    void setCamera(const Mat4& viewProj, Vec3 eye, f32 fadeNear = 20.f, f32 fadeFar = 120.f);
    NodeHandle markerNode(u32 id) const;
    bool markerOffscreen(u32 id) const;

    // device prompts: prompt nodes show the glyph of the last used device
    NodeHandle prompt(NodeHandle parent, Action a, std::string_view caption, std::string_view id = {});
    void noteDevice(InputDevice d);
    InputDevice device() const { return m_device; }

    // sounds
    void setSoundHook(SoundHook fn, void* user) { m_sound = fn; m_soundUser = user; }
    void playSound(UiSound s) { if (m_sound) m_sound(m_soundUser, s); }
    /// Hooks a GamepadNav / focus changes to the sound hook (Move on focus change).
    void watchFocusSounds(bool on) { m_focusSounds = on; }

private:
    struct Float { NodeHandle node; f32 life = 0.f, total = 0.f; Vec2 at; };
    struct FeedLine { NodeHandle node; f32 life = 0.f; };
    struct Marker { NodeHandle node; Vec3 world; bool used = false, off = false; };
    void layoutMarkers();

    UiContext& m_ui;
    NodeHandle m_hud, m_safe, m_feed, m_sub, m_dlg, m_dlgSpeaker, m_dlgBody, m_markers;
    GameClock m_clock;
    Insets m_insets;
    std::array<Float, kFloatPool> m_floats{};
    std::array<FeedLine, kFeedLines> m_lines{};
    std::array<Marker, kMarkerPool> m_marks{};
    std::vector<std::pair<NodeHandle, Action>> m_prompts;
    Mat4 m_vp{};
    Vec3 m_eye;
    f32 m_fadeNear = 20.f, m_fadeFar = 120.f;
    bool m_haveCamera = false;
    f32 m_subLife = 0.f;
    std::string m_dlgText;
    std::size_t m_dlgShown = 0;
    f32 m_dlgCps = 40.f, m_dlgAcc = 0.f;
    bool m_dlgOpen = false;
    InputDevice m_device = InputDevice::KeyboardMouse;
    SoundHook m_sound = nullptr;
    void* m_soundUser = nullptr;
    bool m_focusSounds = false;
    NodeHandle m_lastFocus;
};

} // namespace fuse::ui::game
