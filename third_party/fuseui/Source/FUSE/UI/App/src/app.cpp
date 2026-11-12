// fuse_app (plan UI5): Application, AppWindow, on-demand loop, settings store, command line, single instance.
#include "fuse/app/app.hpp"

#include "fuse/ui/doc/document.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fuse::app {
using namespace fuse::ui;

namespace {
bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && p == s.data() + s.size();
}
double steadyNow(void*) {
    using C = std::chrono::steady_clock;
    static const C::time_point t0 = C::now();
    return std::chrono::duration<double>(C::now() - t0).count();
}
/// Default event source (no OS windows): sleeps for the wait, capped so quit() from a callback is seen.
bool sleepPump(void*, Application&, f32 wait) {
    if (wait > 0.f) std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.25f)));
    return true;
}
std::string escape(std::string_view s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '=') o += "\\e";
        else o += c;
    }
    return o;
}
std::string unescape(std::string_view s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            o += c == 'n' ? '\n' : c == 'e' ? '=' : c;
        } else {
            o += s[i];
        }
    }
    return o;
}
} // namespace

const fuse::text::Font& defaultFont() {
    static fuse::text::Font f = [] {
        auto r = fuse::text::Font::bundled();
        if (!r) { std::fprintf(stderr, "fuse_app: bundled font missing\n"); std::exit(2); }
        return std::move(*r);
    }();
    // Bundled fallback chain (Hebrew, Arabic, Devanagari, colour emoji, symbols), attached once.
    static const std::vector<fuse::text::Font> fallbacks = [] {
        auto v = fuse::text::Font::bundledFallbacks();
        std::vector<const fuse::text::Font*> chain;
        for (const auto& x : v) chain.push_back(&x);
        f.setFallbacks(std::move(chain));
        return v;
    }();
    (void)fallbacks;
    return f;
}

// ---- CommandLine ---------------------------------------------------------------------------------------------------
CommandLine::CommandLine(int argc, const char* const* argv) {
    if (argc > 0 && argv && argv[0]) m_program = argv[0];
    bool opts = true;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i] ? argv[i] : "";
        if (opts && a == "--") { opts = false; continue; }
        if (opts && a.size() > 2 && a.substr(0, 2) == "--") {
            const std::size_t eq = a.find('=');
            if (eq == std::string_view::npos) m_options.emplace_back(std::string(a.substr(2)), "1");
            else m_options.emplace_back(std::string(a.substr(2, eq - 2)), std::string(a.substr(eq + 1)));
        } else {
            m_positional.emplace_back(a);
        }
    }
}
bool CommandLine::has(std::string_view key) const {
    return std::any_of(m_options.begin(), m_options.end(), [&](const auto& o) { return o.first == key; });
}
std::string_view CommandLine::get(std::string_view key, std::string_view fallback) const {
    for (auto it = m_options.rbegin(); it != m_options.rend(); ++it) // last one wins
        if (it->first == key) return it->second;
    return fallback;
}
double CommandLine::number(std::string_view key, double fallback) const {
    double v;
    return parseDouble(get(key), v) ? v : fallback;
}

// ---- Settings ------------------------------------------------------------------------------------------------------
std::string Settings::defaultDir(std::string_view app) {
#if defined(_WIN32)
    const char* base = std::getenv("APPDATA");
    return std::string(base ? base : ".") + "\\" + std::string(app);
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"); x && *x) return std::string(x) + "/" + std::string(app);
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.config/" + std::string(app);
#endif
}
bool Settings::load() {
    m_items.clear();
    m_dirty = false;
    std::ifstream f(m_path, std::ios::binary);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        m_items.emplace_back(unescape(std::string_view(line).substr(0, eq)), unescape(std::string_view(line).substr(eq + 1)));
    }
    return true;
}
bool Settings::save() {
    if (!m_dirty) return true;
    if (m_path.empty()) return false;
    if (const std::size_t slash = m_path.find_last_of("/\\"); slash != std::string::npos) {
        const std::string dir = m_path.substr(0, slash); // create the chain (best effort, no <filesystem>)
        for (std::size_t i = 1; i <= dir.size(); ++i)
            if (i == dir.size() || dir[i] == '/' || dir[i] == '\\') {
                const std::string part = dir.substr(0, i);
#if defined(_WIN32)
                CreateDirectoryA(part.c_str(), nullptr);
#else
                ::mkdir(part.c_str(), 0700);
#endif
            }
    }
    const std::string tmp = m_path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        for (const auto& [k, v] : m_items) f << escape(k) << '=' << escape(v) << '\n';
        if (!f) return false;
    }
#if defined(_WIN32)
    if (!MoveFileExA(tmp.c_str(), m_path.c_str(), MOVEFILE_REPLACE_EXISTING)) return false;
#else
    if (std::rename(tmp.c_str(), m_path.c_str()) != 0) return false;
#endif
    m_dirty = false;
    return true;
}
std::string_view Settings::get(std::string_view key, std::string_view fallback) const {
    for (const auto& [k, v] : m_items)
        if (k == key) return v;
    return fallback;
}
double Settings::number(std::string_view key, double fallback) const {
    double v;
    return parseDouble(get(key), v) ? v : fallback;
}
void Settings::set(std::string_view key, std::string_view value) {
    for (auto& [k, v] : m_items)
        if (k == key) {
            if (v != value) { v = value; m_dirty = true; }
            return;
        }
    m_items.emplace_back(std::string(key), std::string(value));
    m_dirty = true;
}
void Settings::set(std::string_view key, double value) {
    char buf[64];
    const auto [p, ec] = std::to_chars(buf, buf + sizeof buf, value);
    set(key, std::string_view(buf, ec == std::errc{} ? std::size_t(p - buf) : 0));
}
void Settings::remove(std::string_view key) {
    const auto it = std::find_if(m_items.begin(), m_items.end(), [&](const auto& kv) { return kv.first == key; });
    if (it != m_items.end()) { m_items.erase(it); m_dirty = true; }
}

// ---- AppWindow -----------------------------------------------------------------------------------------------------
AppWindow::AppWindow(Application& app, const WindowDesc& d, u32 id)
    : m_app(app), m_ui({.viewport = {f32(d.width), f32(d.height)}, .font = &defaultFont()}), m_title(d.title), m_id(id) {
    widgets::install(m_ui);
    m_presenter = {this, &AppWindow::presentHeadless, nullptr};
}
AppWindow::~AppWindow() {
    m_doc.reset();
    if (m_presenter.destroy) m_presenter.destroy(m_presenter.user);
}
void AppWindow::setPresenter(const Presenter& p) {
    if (m_presenter.destroy) m_presenter.destroy(m_presenter.user);
    m_presenter = p.present ? p : Presenter{this, &AppWindow::presentHeadless, nullptr};
    m_forceRedraw = true;
}
void AppWindow::resize(u32 width, u32 height) {
    m_ui.setViewport({f32(width), f32(height)});
    m_ui.markAllDirty();
    m_forceRedraw = true;
}
std::expected<void, std::string> AppWindow::loadText(std::string_view json, doc::Model* model) {
    m_doc.reset();
    auto d = doc::Document::load(m_ui, json, model);
    if (!d) {
        const auto& e = d.error();
        return std::unexpected(e.path + ": " + e.message + (e.line ? " (line " + std::to_string(e.line) + ")" : ""));
    }
    m_doc = std::move(*d);
    m_forceRedraw = true;
    return {};
}
std::expected<void, std::string> AppWindow::load(std::string_view path, doc::Model* model) {
    std::ifstream f{std::string(path), std::ios::binary};
    if (!f) return std::unexpected("cannot open " + std::string(path));
    std::stringstream ss;
    ss << f.rdbuf();
    return loadText(ss.str(), model);
}
bool AppWindow::presentHeadless(void*, AppWindow& w, Rect damage) {
    const int iw = int(w.m_ui.viewport().w), ih = int(w.m_ui.viewport().h);
    const bool full = w.m_image.width != iw || w.m_image.height != ih;
    if (full) w.m_image.resize(iw, ih, 0xFF000000u);
    w.m_raster.setGlyphAtlas(w.m_ui.atlas().pixels(), w.m_ui.atlas().size());
    const Rect r = full || damage.empty() ? Rect{0.f, 0.f, f32(iw), f32(ih)} : damage;
    // clear the damaged region, then rasterise the list clipped to it (incremental present)
    const int x0 = std::max(0, int(std::floor(r.x))), y0 = std::max(0, int(std::floor(r.y)));
    const int x1 = std::min(iw, int(std::ceil(r.x + r.w))), y1 = std::min(ih, int(std::ceil(r.y + r.h)));
    for (int y = y0; y < y1; ++y)
        std::fill_n(w.m_image.pixels.begin() + std::ptrdiff_t(y) * iw + x0, std::max(0, x1 - x0), 0xFF000000u);
    w.m_raster.render(w.m_ui.drawList(), w.m_image, {f32(x0), f32(y0), f32(x1 - x0), f32(y1 - y0)});
    return true;
}
bool writePng(const Image& img, const std::string& path) {
    if (img.width <= 0) return false;
    return stbi_write_png(path.c_str(), img.width, img.height, 4, img.pixels.data(), img.width * 4) != 0;
}
bool AppWindow::writePng(const std::string& path) const {
    if (m_image.width <= 0) return false;
    return stbi_write_png(path.c_str(), m_image.width, m_image.height, 4, m_image.pixels.data(), m_image.width * 4) != 0;
}
bool AppWindow::frame(f32 dt) {
    if (!m_forceRedraw && m_policy == RedrawPolicy::OnDemand && !m_ui.needsUpdate()) return false;
    m_ui.update(dt); // advances timers; presents only when something repainted
    if (m_doc && m_doc->refresh()) m_ui.update(0.f);
    if (!m_ui.stats().repainted && !m_forceRedraw && m_policy == RedrawPolicy::OnDemand) return false;
    const Rect dmg = m_forceRedraw ? Rect{0.f, 0.f, m_ui.viewport().w, m_ui.viewport().h} : m_ui.damage();
    m_forceRedraw = false;
    if (!m_presenter.present || !m_presenter.present(m_presenter.user, *this, dmg)) return false;
    ++m_frames;
    return true;
}

// ---- Application ---------------------------------------------------------------------------------------------------
Application::Application(const AppDesc& d) : m_name(d.name), m_args(d.argc, d.argv) {
    m_events = {nullptr, &sleepPump};
    m_clock = {nullptr, &steadyNow};
    if (d.loadSettings) {
        const std::string_view over = m_args.get("settings");
        m_settings.setPath(over.empty() ? Settings::defaultDir(m_name) + "/settings.ini" : std::string(over));
        m_settings.load();
    }
    if (d.singleInstance) {
#if defined(_WIN32)
        const std::string mx = "Local\\fuse_app_" + m_name;
        HANDLE h = CreateMutexA(nullptr, TRUE, mx.c_str());
        m_primary = h && GetLastError() != ERROR_ALREADY_EXISTS;
        m_lock = h;
#else
        const char* rt = std::getenv("XDG_RUNTIME_DIR");
        const std::string path = std::string(rt && *rt ? rt : "/tmp") + "/fuse_app_" + m_name + "_" + std::to_string(::getuid()) + ".lock";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        m_primary = fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (fd >= 0) m_lock = reinterpret_cast<void*>(std::intptr_t(fd) + 1);
#endif
    }
}
Application::~Application() {
    m_windows.clear();
    m_settings.save();
#if defined(_WIN32)
    if (m_lock) CloseHandle(static_cast<HANDLE>(m_lock));
#else
    if (m_lock) ::close(int(reinterpret_cast<std::intptr_t>(m_lock) - 1)); // releases the flock
#endif
}
AppWindow& Application::createWindow(const WindowDesc& d) {
    m_windows.push_back(std::make_unique<AppWindow>(*this, d, m_nextId++));
    return *m_windows.back();
}
void Application::setEventSource(const EventSource& s) { m_events = s.pump ? s : EventSource{nullptr, &sleepPump}; }
double Application::now() const { return m_clock.now(m_clock.user); }
f32 Application::nextWake() const {
    f32 t = std::numeric_limits<f32>::infinity();
    for (const auto& w : m_windows) {
        if (w->m_forceRedraw || w->m_closing || w->m_policy == RedrawPolicy::Continuous) return 0.f;
        t = std::min(t, w->m_ui.nextWakeSeconds());
    }
    return t;
}
bool Application::step(f32 maxWait) {
    ++m_stats.iterations;
    if (m_quit) return false;
    const f32 wait = std::min(nextWake(), maxWait);
    const double before = now();
    if (m_events.pump && !m_events.pump(m_events.user, *this, wait)) m_quit = true;
    const double t = now();
    if (wait > 0.f && t > before) { ++m_stats.sleeps; m_stats.sleptSeconds += t - before; }
    // no clamp: after an idle sleep the elapsed time is real and UI timers must see all of it
    const f32 dt = m_last < 0.0 ? 0.f : f32(std::max(0.0, t - m_last));
    m_last = t;
    for (auto& w : m_windows)
        if (!w->m_closing && w->frame(dt)) ++m_stats.frames;
    std::erase_if(m_windows, [](const auto& w) { return w->m_closing; });
    if (m_windows.empty()) m_quit = true;
    return !m_quit;
}
int Application::run() {
    while (step()) {
    }
    return m_exit;
}
int Application::fail(std::string_view message) {
    std::fprintf(stderr, "%s: %.*s\n", m_name.c_str(), int(message.size()), message.data());
    m_exit = 1;
    return 1;
}

} // namespace fuse::app
