// fuse_app (plan UI5): the application shell for UI-only projects. An Application owns AppWindows (each a UiContext
// plus a presenter), command-line arguments, a settings store and the single-instance lock, and runs an on-demand
// loop: it sleeps until input, a UI timer or an animation needs a frame (UiContext::nextWakeSeconds), so an idle app
// presents nothing. There is no second loop: an engine host calls step() from its frame instead of run().
//
// Backends are function tables (no inheritance): an EventSource pumps OS input into the windows and blocks for at
// most the wait it is given; a Presenter shows a window's draw list. The built-in headless backend rasterises on the
// CPU into the window's image (tests, screenshots, servers); desktop / mobile presenters plug in the same way.
#pragma once

#include "fuse/ui/ui.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::ui::doc {
class Document;
class Model;
} // namespace fuse::ui::doc

namespace fuse::app {

using fuse::ui::f32;
using fuse::ui::u32;
using fuse::ui::u64;

// ---- command line --------------------------------------------------------------------------------------------------
/// `--key=value`, `--key value` is NOT split (ambiguous); `--flag` = "1"; `--` ends options; everything else positional.
class CommandLine {
public:
    CommandLine() = default;
    CommandLine(int argc, const char* const* argv);
    bool has(std::string_view key) const;
    std::string_view get(std::string_view key, std::string_view fallback = {}) const;
    double number(std::string_view key, double fallback) const;
    const std::vector<std::string>& positional() const { return m_positional; }
    std::string_view program() const { return m_program; }

private:
    std::string m_program;
    std::vector<std::pair<std::string, std::string>> m_options;
    std::vector<std::string> m_positional;
};

// ---- settings ------------------------------------------------------------------------------------------------------
/// Flat key = value store persisted as UTF-8 text (one `key=value` per line, '\n' and '\\' escaped). Saved by save()
/// and when the Application exits, only if something changed.
class Settings {
public:
    void setPath(std::string path) { m_path = std::move(path); }
    const std::string& path() const { return m_path; }
    bool load();       ///< false when the file is missing or unreadable (the store is then empty)
    bool save();       ///< writes atomically (temp + rename); true when clean or written
    bool dirty() const { return m_dirty; }
    std::string_view get(std::string_view key, std::string_view fallback = {}) const;
    double number(std::string_view key, double fallback = 0.0) const;
    bool flag(std::string_view key, bool fallback = false) const { return number(key, fallback ? 1.0 : 0.0) != 0.0; }
    void set(std::string_view key, std::string_view value);
    void set(std::string_view key, double value);
    void remove(std::string_view key);
    u32 size() const { return u32(m_items.size()); }
    /// Platform config directory for an app: $XDG_CONFIG_HOME/<app> (~/.config/<app>), %APPDATA%\<app>.
    static std::string defaultDir(std::string_view app);

private:
    std::vector<std::pair<std::string, std::string>> m_items;
    std::string m_path;
    bool m_dirty = false;
};

// ---- windows -------------------------------------------------------------------------------------------------------
struct WindowDesc {
    std::string_view title = "FUSE";
    u32 width = 800, height = 600;
    f32 dpiScale = 1.f;
};
enum class RedrawPolicy : fuse::ui::u8 {
    OnDemand,   ///< update + present only when the UI needs it (default)
    Continuous, ///< every loop iteration (games, live viewports)
};

class Application;
class AppWindow;

/// Shows a window's frame. `damage` is the union of changed rects (whole window on the first frame / resize).
struct Presenter {
    void* user = nullptr;
    bool (*present)(void* user, AppWindow& w, fuse::ui::Rect damage) = nullptr;
    void (*destroy)(void* user) = nullptr; ///< called when the window closes
};

class AppWindow {
public:
    AppWindow(Application& app, const WindowDesc& d, u32 id);
    ~AppWindow();
    AppWindow(const AppWindow&) = delete;
    AppWindow& operator=(const AppWindow&) = delete;

    fuse::ui::UiContext& ui() { return m_ui; }
    Application& app() { return m_app; }
    u32 id() const { return m_id; }
    const std::string& title() const { return m_title; }
    void setTitle(std::string_view t) { m_title = t; }
    /// Loads a .fuseui document (file path) into the window root, replacing a previous one. `model` may be null.
    std::expected<void, std::string> load(std::string_view path, fuse::ui::doc::Model* model = nullptr);
    std::expected<void, std::string> loadText(std::string_view json, fuse::ui::doc::Model* model = nullptr);
    fuse::ui::doc::Document* document() { return m_doc.get(); }

    void resize(u32 width, u32 height); ///< logical units; forces a full repaint
    fuse::ui::Size size() const { return m_ui.viewport(); }
    void setRedrawPolicy(RedrawPolicy p) { m_policy = p; }
    RedrawPolicy redrawPolicy() const { return m_policy; }
    void setPresenter(const Presenter& p);
    /// Frames presented so far (the idle gate checks this stays flat).
    u64 framesPresented() const { return m_frames; }
    void requestClose() { m_closing = true; }
    bool closing() const { return m_closing; }
    void requestRedraw() { m_forceRedraw = true; }
    /// Headless presenter target: the last CPU-rasterised frame.
    const fuse::ui::Image& image() const { return m_image; }
    bool writePng(const std::string& path) const;
    /// Registers an app-owned image with the CPU rasteriser (viewportWidget / image index). The image must outlive the
    /// window; call ui().invalidate(node) after changing its pixels.
    u32 addImage(const fuse::ui::Image* img) { return m_raster.addImage(img); }
    const fuse::ui::CpuRasterizer& rasterizer() const { return m_raster; }
    /// CPU-rasterises `damage` (whole window when empty / resized) into image(): the headless presenter, also used by
    /// native presenters that upload the CPU raster.
    const fuse::ui::Image& rasterize(fuse::ui::Rect damage) {
        presentHeadless(nullptr, *this, damage);
        return m_image;
    }

    // input (backends call these; they forward to the UiContext and wake the loop)
    void pointerMove(fuse::ui::Vec2 p) { m_ui.pointerMove(p); }
    void pointerDown(fuse::ui::Vec2 p) { m_ui.pointerDown(p); }
    void pointerUp(fuse::ui::Vec2 p) { m_ui.pointerUp(p); }
    void click(fuse::ui::Vec2 p) { m_ui.pointerMove(p); m_ui.pointerDown(p); m_ui.pointerUp(p); }
    void key(fuse::ui::Key k, fuse::ui::u8 mods = 0) { m_ui.keyDown(k, mods); m_ui.keyUp(k, mods); }
    void text(std::string_view utf8) { m_ui.textInput(utf8); }

private:
    friend class Application;
    bool frame(f32 dt); ///< update + present when needed; true when presented
    static bool presentHeadless(void* user, AppWindow& w, fuse::ui::Rect damage);

    Application& m_app;
    fuse::ui::UiContext m_ui;
    std::unique_ptr<fuse::ui::doc::Document> m_doc;
    std::string m_title;
    Presenter m_presenter;
    fuse::ui::Image m_image;
    fuse::ui::CpuRasterizer m_raster;
    RedrawPolicy m_policy = RedrawPolicy::OnDemand;
    u64 m_frames = 0;
    u32 m_id;
    bool m_closing = false, m_forceRedraw = true;
};

// ---- application ---------------------------------------------------------------------------------------------------
struct AppDesc {
    std::string_view name = "fuse_app";
    int argc = 0;
    const char* const* argv = nullptr;
    bool singleInstance = false;   ///< take the per-user instance lock (primary() is false when another holds it)
    bool loadSettings = true;      ///< read <config dir>/settings.ini at start (--settings=<path> overrides)
};

/// Pumps OS events into the windows, blocking for at most `waitSeconds` (+inf = until an event). Returns false to stop.
struct EventSource {
    void* user = nullptr;
    bool (*pump)(void* user, Application& app, f32 waitSeconds) = nullptr;
};
/// Monotonic clock in seconds (tests drive a virtual one).
struct Clock {
    void* user = nullptr;
    double (*now)(void* user) = nullptr;
};

struct LoopStats {
    u64 iterations = 0, frames = 0, sleeps = 0;
    double sleptSeconds = 0.0;
};

class Application {
public:
    explicit Application(const AppDesc& d);
    ~Application(); ///< saves dirty settings, releases the instance lock
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    AppWindow& createWindow(const WindowDesc& d);
    u32 windowCount() const { return u32(m_windows.size()); }
    AppWindow* window(u32 index) { return index < m_windows.size() ? m_windows[index].get() : nullptr; }

    const CommandLine& args() const { return m_args; }
    Settings& settings() { return m_settings; }
    const std::string& name() const { return m_name; }
    bool primary() const { return m_primary; } ///< false when singleInstance and another instance runs

    /// Replaces the event pump (an empty source restores the default sleep pump).
    void setEventSource(const EventSource& s);
    void setClock(const Clock& c) { m_clock = c; }
    /// One loop iteration: pump events (waiting at most until the next UI deadline, capped by `maxWait`), then
    /// update / present every window that needs it and drop closed windows. False once the app should exit.
    bool step(f32 maxWait = 1e30f);
    /// Runs until quit() or the last window closes. Returns the exit code.
    int run();
    void quit(int code = 0) { m_quit = true; m_exit = code; }
    bool quitting() const { return m_quit; }
    /// Prints `message` to stderr and returns the failure exit code (for `return app.fail(err)`).
    int fail(std::string_view message);
    const LoopStats& stats() const { return m_stats; }
    /// Seconds until some window needs a frame (0 = now, +inf = idle).
    f32 nextWake() const;
    double now() const;

private:
    std::string m_name;
    CommandLine m_args;
    Settings m_settings;
    std::vector<std::unique_ptr<AppWindow>> m_windows;
    EventSource m_events;
    Clock m_clock;
    LoopStats m_stats;
    double m_last = -1.0;
    void* m_lock = nullptr; ///< platform instance-lock handle
    u32 m_nextId = 1;
    int m_exit = 0;
    bool m_quit = false, m_primary = true;
};

/// Writes an RGBA8 image as PNG.
bool writePng(const fuse::ui::Image& image, const std::string& path);

/// The bundled font every window uses by default (loaded once).
const fuse::text::Font& defaultFont();

} // namespace fuse::app
