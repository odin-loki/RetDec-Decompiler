// fuse_app native windows: X11 (libX11 loaded at runtime, so neither X11 headers nor libX11 are needed to build) and
// Win32, plus the presenter selection (Vulkan swapchain -> GDI on Win32 -> headless). See native.hpp.
#include "native_internal.hpp"
#include "fuse/ui/widgets/widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <poll.h>
#endif

namespace fuse::app {
using namespace fuse::ui;
using detail::SurfaceSource;

#if !defined(FUSE_APP_HAS_VULKAN)
// Built without a Vulkan loader: no swapchain presenter.
namespace detail {
struct VkCtx {};
struct VkWin {};
VkCtx* vkCtxCreate(SurfaceSource::Kind, std::string& err) { err = "built without Vulkan"; return nullptr; }
VkCtx* vkCtxAdopt(SurfaceSource::Kind, const VulkanDeviceHandles&, std::string& err) { err = "built without Vulkan"; return nullptr; }
void vkCtxDestroy(VkCtx*) {}
bool vkWinSetExternal(VkWin*, unsigned, const ExternalTexture&) { return false; }
const char* vkCtxDeviceName(const VkCtx*) { return ""; }
VkWin* vkWinCreate(VkCtx*, const SurfaceSource&, const NativeOptions&, std::string&) { return nullptr; }
bool vkWinPresent(VkWin*, AppWindow&, Rect) { return false; }
bool vkWinGpuDraws(const VkWin*) { return false; }
void vkWinDestroy(VkWin*) {}
bool vkWinReadback(VkWin*, Image&) { return false; }
} // namespace detail
#endif

namespace {
u8 asciiKey(Key& k, u32 c) {
    switch (c) {
    case 'a': case 'A': k = Key::A; return 1;
    case 'c': case 'C': k = Key::C; return 1;
    case 'v': case 'V': k = Key::V; return 1;
    case 'x': case 'X': k = Key::X; return 1;
    case 'y': case 'Y': k = Key::Y; return 1;
    case 'z': case 'Z': k = Key::Z; return 1;
    case 'w': case 'W': k = Key::W; return 1;
    case 's': case 'S': k = Key::S; return 1;
    case 'd': case 'D': k = Key::D; return 1;
    case 'q': case 'Q': k = Key::Q; return 1;
    case 'e': case 'E': k = Key::E; return 1;
    case 'n': case 'N': k = Key::N; return 1;
    case 'o': case 'O': k = Key::O; return 1;
    case ' ': k = Key::Space; return 1;
    case '1': k = Key::Num1; return 1;
    case '2': k = Key::Num2; return 1;
    case '3': k = Key::Num3; return 1;
    case '4': k = Key::Num4; return 1;
    case 'b': case 'B': k = Key::B; return 1;
    case 'f': case 'F': k = Key::F; return 1;
    case 'g': case 'G': k = Key::G; return 1;
    case 'h': case 'H': k = Key::H; return 1;
    case 'i': case 'I': k = Key::I; return 1;
    case 'j': case 'J': k = Key::J; return 1;
    case 'k': case 'K': k = Key::K; return 1;
    case 'l': case 'L': k = Key::L; return 1;
    case 'm': case 'M': k = Key::M; return 1;
    case 'p': case 'P': k = Key::P; return 1;
    case 'r': case 'R': k = Key::R; return 1;
    case 't': case 'T': k = Key::T; return 1;
    case 'u': case 'U': k = Key::U; return 1;
    case '0': k = Key::Num0; return 1;
    case '5': k = Key::Num5; return 1;
    case '6': k = Key::Num6; return 1;
    case '7': k = Key::Num7; return 1;
    case '8': k = Key::Num8; return 1;
    case '9': k = Key::Num9; return 1;
    case '[': k = Key::BracketLeft; return 1;
    case ']': k = Key::BracketRight; return 1;
    default: return 0;
    }
}
void appendUtf8(std::string& s, u32 cp) {
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
    else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 0x3F)); s += char(0x80 | ((cp >> 6) & 0x3F)); s += char(0x80 | (cp & 0x3F)); }
}
} // namespace

#if !defined(_WIN32)
// ---- X11 (runtime-loaded) ----------------------------------------------------------------------------------------
namespace x11 {
using Display = struct _XDisplay;
using XID = unsigned long;
using Window = XID;
using Atom = unsigned long;
using KeySym = XID;
// Event layouts (LP64 Xlib ABI); XEvent is a union padded to 24 longs.
struct AnyEv { int type; unsigned long serial; int send_event; Display* display; Window window; };
struct InputEv {
    int type; unsigned long serial; int send_event; Display* display; Window window, root, subwindow;
    unsigned long time; int x, y, x_root, y_root; unsigned state; unsigned detail; // keycode / button
    int same_screen;
};
struct ConfigureEv { int type; unsigned long serial; int send_event; Display* display; Window event, window; int x, y, width, height; };
struct ClientEv { int type; unsigned long serial; int send_event; Display* display; Window window; Atom message_type; int format; long l[5]; };
union Event { int type; AnyEv any; InputEv input; ConfigureEv configure; ClientEv client; long pad[24]; };
struct Image;
struct ImageFuncs { Image* (*create)(); int (*destroy)(Image*); };
struct Image {
    int width, height, xoffset, format; char* data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth,
        bytes_per_line, bits_per_pixel; unsigned long red_mask, green_mask, blue_mask; char* obdata; ImageFuncs f;
};
enum { KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6, FocusOut = 10, Expose = 12,
       MapNotify = 19, ConfigureNotify = 22, ClientMessage = 33 };
constexpr long kMask = 1 | 2 | 4 | 8 | (1 << 6) | (1 << 15) | (1 << 17) | (1 << 21);

struct Lib {
    void* so = nullptr;
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(Display*) = nullptr;
    int (*DefaultScreen)(Display*) = nullptr;
    Window (*RootWindow)(Display*, int) = nullptr;
    Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned, unsigned, unsigned, unsigned long, unsigned long) = nullptr;
    int (*DestroyWindow)(Display*, Window) = nullptr;
    int (*StoreName)(Display*, Window, const char*) = nullptr;
    int (*SelectInput)(Display*, Window, long) = nullptr;
    int (*MapWindow)(Display*, Window) = nullptr;
    Atom (*InternAtom)(Display*, const char*, int) = nullptr;
    int (*SetWMProtocols)(Display*, Window, Atom*, int) = nullptr;
    int (*Pending)(Display*) = nullptr;
    int (*NextEvent)(Display*, Event*) = nullptr;
    int (*ConnectionNumber)(Display*) = nullptr;
    int (*Flush)(Display*) = nullptr;
    int (*Sync)(Display*, int) = nullptr;
    int (*LookupString)(InputEv*, char*, int, KeySym*, void*) = nullptr;
    Image* (*GetImage)(Display*, XID, int, int, unsigned, unsigned, unsigned long, int) = nullptr;
    void* (*SetErrorHandler)(int (*)(Display*, void*)) = nullptr;
    int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*, int) = nullptr;
    int (*GetWindowProperty)(Display*, Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*, unsigned long*,
                             unsigned char**) = nullptr;
    int (*Free)(void*) = nullptr;
    int (*ResizeWindow)(Display*, Window, unsigned, unsigned) = nullptr;
    Image* (*CreateImage)(Display*, void*, unsigned, int, int, char*, unsigned, unsigned, int, int) = nullptr;
    int (*PutImage)(Display*, XID, void*, Image*, int, int, int, int, unsigned, unsigned) = nullptr;
    void* (*DefaultGC)(Display*, int) = nullptr;
    void* (*DefaultVisual)(Display*, int) = nullptr;
    int (*DefaultDepth)(Display*, int) = nullptr;
    int (*MoveResizeWindow)(Display*, Window, int, int, unsigned, unsigned) = nullptr;
    int (*TranslateCoordinates)(Display*, Window, Window, int, int, int*, int*, Window*) = nullptr;
    int (*ChangeWindowAttributes)(Display*, Window, unsigned long, void*) = nullptr;
    int (*DisplayWidth)(Display*, int) = nullptr;
    int (*DisplayHeight)(Display*, int) = nullptr;
    int (*SendEvent)(Display*, Window, int, long, Event*) = nullptr;
    int (*GrabPointer)(Display*, Window, int, unsigned, int, int, Window, XID, unsigned long) = nullptr;
    int (*UngrabPointer)(Display*, unsigned long) = nullptr;

    bool load() {
        so = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!so) so = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        bool all = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(so, name));
            all = all && fn;
        };
        get(OpenDisplay, "XOpenDisplay"); get(CloseDisplay, "XCloseDisplay"); get(DefaultScreen, "XDefaultScreen");
        get(RootWindow, "XRootWindow"); get(CreateSimpleWindow, "XCreateSimpleWindow"); get(DestroyWindow, "XDestroyWindow");
        get(StoreName, "XStoreName"); get(SelectInput, "XSelectInput"); get(MapWindow, "XMapWindow");
        get(InternAtom, "XInternAtom"); get(SetWMProtocols, "XSetWMProtocols"); get(Pending, "XPending");
        get(NextEvent, "XNextEvent"); get(ConnectionNumber, "XConnectionNumber"); get(Flush, "XFlush"); get(Sync, "XSync");
        get(LookupString, "XLookupString"); get(GetImage, "XGetImage"); get(SetErrorHandler, "XSetErrorHandler");
        get(ChangeProperty, "XChangeProperty"); get(GetWindowProperty, "XGetWindowProperty"); get(Free, "XFree"); get(ResizeWindow, "XResizeWindow");
        get(CreateImage, "XCreateImage"); get(PutImage, "XPutImage"); get(DefaultGC, "XDefaultGC");
        get(DefaultVisual, "XDefaultVisual"); get(DefaultDepth, "XDefaultDepth"); get(MoveResizeWindow, "XMoveResizeWindow");
        get(TranslateCoordinates, "XTranslateCoordinates"); get(ChangeWindowAttributes, "XChangeWindowAttributes");
        get(DisplayWidth, "XDisplayWidth"); get(DisplayHeight, "XDisplayHeight"); get(SendEvent, "XSendEvent");
        get(GrabPointer, "XGrabPointer"); get(UngrabPointer, "XUngrabPointer");
        return all;
    }
};
// XSetWindowAttributes (LP64 Xlib ABI)
struct SetWindowAttributes {
    unsigned long background_pixmap, background_pixel, border_pixmap, border_pixel;
    int bit_gravity, win_gravity, backing_store;
    unsigned long backing_planes, backing_pixel;
    int save_under;
    long event_mask, do_not_propagate_mask;
    int override_redirect;
    unsigned long colormap, cursor;
};
constexpr unsigned long kCWOverrideRedirect = 1ul << 9, kCWSaveUnder = 1ul << 10;
constexpr long kPopupMask = (1 << 2) | (1 << 3) | (1 << 6) | (1 << 15); // buttons, motion, exposure
int ignoreErrors(Display*, void*) { return 0; } // a capture of an unmapped window must not abort the process

Key mapKey(KeySym s, bool& ascii) {
    ascii = false;
    switch (s) {
    case 0xff09: case 0xfe20: return Key::Tab;
    case 0xff0d: case 0xff8d: return Key::Enter;
    case 0xff1b: return Key::Escape;
    case 0xff08: return Key::Backspace;
    case 0xffff: return Key::Delete;
    case 0xff51: return Key::Left;
    case 0xff52: return Key::Up;
    case 0xff53: return Key::Right;
    case 0xff54: return Key::Down;
    case 0xff50: return Key::Home;
    case 0xff57: return Key::End;
    case 0xff55: return Key::PageUp;
    case 0xff56: return Key::PageDown;
    case 0xffbf: return Key::F2;
    case 0xffc2: return Key::F5;
    case 0xffc7: return Key::F10;
    case 0xffbe: return Key::F1;
    case 0xffc0: return Key::F3;
    case 0xffc1: return Key::F4;
    case 0xffc3: return Key::F6;
    case 0xffc4: return Key::F7;
    case 0xffc5: return Key::F8;
    case 0xffc6: return Key::F9;
    case 0xffc8: return Key::F11;
    case 0xffc9: return Key::F12;
    default: break;
    }
    Key k = Key::Other;
    ascii = s < 0x80 && asciiKey(k, u32(s));
    return k;
}
u8 mods(unsigned state) {
    return u8(((state & 1) ? kModShift : 0) | ((state & 4) ? kModCtrl : 0) | ((state & 8) ? kModAlt : 0) |
              ((state & 64) ? kModSuper : 0));
}
} // namespace x11
#endif

// ---- shared ----------------------------------------------------------------------------------------------------------
struct NativePlatform::Impl {
    struct Rec {
        Impl* impl = nullptr;
        AppWindow* win = nullptr;
        detail::VkWin* vk = nullptr;
        PresenterKind kind = PresenterKind::Headless;
        unsigned long long handle = 0; ///< X11 Window / HWND
        bool mapped = false;
        bool focused = false;
        struct Popup {
            unsigned long long handle = 0;
            Rect screen;   ///< physical screen rect
            Rect rect;     ///< context rect (input offset)
            u64 revision = ~0ull;
            Image img;
            std::vector<u32> staged; ///< OS pixel order
        };
        std::vector<Popup> popups;
#if defined(_WIN32)
        std::vector<u32> bgra; ///< GDI fallback staging
        wchar_t highSurrogate = 0;
#endif
    };

    Application& app;
    NativeOptions opts;
    std::vector<Rec*> recs;
    detail::VkCtx* vk = nullptr;
    bool vkTried = false;
    std::string error;
    EventSource previous{};
#if defined(_WIN32)
    HINSTANCE hinst = nullptr;
#else
    x11::Lib x;
    x11::Display* dpy = nullptr;
    x11::Atom wmProtocols = 0, wmDelete = 0;
#endif

    Impl(Application& a, const NativeOptions& o) : app(a), opts(o) {}

    Rec* find(unsigned long long h) {
        for (Rec* r : recs)
            if (r->handle == h) return r;
        return nullptr;
    }
    const Rec* find(const AppWindow& w) const {
        for (const Rec* r : recs)
            if (r->win == &w) return r;
        return nullptr;
    }
    detail::VkCtx* vkContext() {
        if (!vkTried && opts.vulkan) {
            vkTried = true;
#if defined(_WIN32)
            const auto kind = SurfaceSource::Win32;
#else
            const auto kind = SurfaceSource::Xlib;
#endif
            vk = opts.adoptDevice.instance && opts.adoptDevice.device ? detail::vkCtxAdopt(kind, opts.adoptDevice, error)
                                                                      : detail::vkCtxCreate(kind, error);
        }
        return vk;
    }

    static bool present(void* user, AppWindow& w, Rect damage);
    static void destroy(void* user);
    static bool pump(void* user, Application& app, f32 wait);
    void destroyOsWindow(Rec& r);
    bool createOsWindow(Rec& r);
    bool captureOs(const Rec& r, Image& out);
    // OS-window popups
    void syncPopups(Rec& r);
    /// An outside click (grabbed pointer outside every window of the app, or the app deactivated): close every popup.
    static void closeAllPopups(Rec& r) {
        UiContext& ui = r.win->ui();
        std::vector<NodeHandle> nodes;
        for (const auto& ps : ui.popupSurfaces()) nodes.push_back(ps.node);
        for (NodeHandle n : nodes)
            if (ui.alive(n)) fuse::ui::widgets::closePopup(ui, n);
        r.win->requestRedraw();
    }
    void grabPointer(Rec& r, bool on);
    bool screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const; ///< client origin + work area (screen px)
    bool createPopupOs(Rec& r, Rec::Popup& p);
    void movePopupOs(Rec::Popup& p);
    void destroyPopupOs(Rec::Popup& p);
    void blitPopupOs(Rec::Popup& p);
    bool capturePopupOs(const Rec::Popup& p, Image& out);
    bool postClickOs(unsigned long long h, Vec2 at);
#if !defined(_WIN32)
    bool captureX(unsigned long win, unsigned w, unsigned h, Image& out);
#endif
    Rec* findPopup(unsigned long long h, std::size_t& idx) {
        for (Rec* r : recs)
            for (std::size_t i = 0; i < r->popups.size(); ++i)
                if (r->popups[i].handle == h) { idx = i; return r; }
        return nullptr;
    }
};

void NativePlatform::Impl::syncPopups(Rec& r) {
    if (!opts.osPopups || !r.handle) return;
    UiContext& ui = r.win->ui();
    int ox = 0, oy = 0;
    Rect work;
    if (screenInfo(r, ox, oy, work)) {
        const f32 s = 1.f; // context px = physical / scale
        ui.setOsPopups(true, {(work.x - f32(ox)) / s, (work.y - f32(oy)) / s, work.w / s, work.h / s});
    }
    const auto& surf = ui.popupSurfaces();
    const bool had = !r.popups.empty();
    while (r.popups.size() > surf.size()) {
        destroyPopupOs(r.popups.back());
        r.popups.pop_back();
    }
    if (had && r.popups.empty()) grabPointer(r, false);
    const f32 s = 1.f;
    for (std::size_t i = 0; i < surf.size(); ++i) {
        if (i == r.popups.size()) r.popups.emplace_back();
        Rec::Popup& p = r.popups[i];
        const auto& ps = surf[i];
        const Rect scr{std::round(f32(ox) + ps.rect.x * s), std::round(f32(oy) + ps.rect.y * s), std::max(1.f, std::ceil(ps.rect.w * s)),
                       std::max(1.f, std::ceil(ps.rect.h * s))};
        const bool moved = scr.x != p.screen.x || scr.y != p.screen.y || scr.w != p.screen.w || scr.h != p.screen.h;
        p.rect = ps.rect;
        p.screen = scr;
        if (!p.handle) {
            if (!createPopupOs(r, p)) { r.popups.pop_back(); return; }
            if (i == 0) grabPointer(r, true); // Qt::Popup: clicks anywhere else on screen close the menu
        } else if (moved) {
            movePopupOs(p);
        }
        if (moved || p.revision != ps.revision) {
            p.revision = ps.revision;
            u32 clear = 0xFF000000u; // the panel's own fill shows in the corners its radius leaves
            for (const DrawCmd& c : ps.draw.cmds())
                if (c.op != DrawOp::Shadow && c.fill.a > 0.9f) { clear = packPremultiplied(c.fill) | 0xFF000000u; break; }
            p.img.resize(int(scr.w), int(scr.h), clear);
            CpuRasterizer rz = r.win->rasterizer(); // the window's images + the current glyph atlas
            rz.setGlyphAtlas(ui.atlas().pixels(), ui.atlas().size());
            if (s == 1.f) {
                rz.render(ps.draw, p.img);
            } else {
                DrawList scaled = ps.draw;
                for (DrawCmd& c : scaled.cmds()) { c.rect.x *= s; c.rect.y *= s; c.rect.w *= s; c.rect.h *= s; }
                rz.render(scaled, p.img);
            }
            blitPopupOs(p);
        }
    }
}

bool NativePlatform::Impl::present(void* user, AppWindow& w, Rect damage) {
    Rec& r = *static_cast<Rec*>(user);
    switch (r.kind) {
    case PresenterKind::Vulkan: {
        const bool ok = detail::vkWinPresent(r.vk, w, damage);
        r.impl->syncPopups(r);
        return ok;
    }
#if defined(_WIN32)
    case PresenterKind::Software: {
        const Image& img = w.rasterize(damage);
        r.bgra.resize(img.pixels.size());
        for (std::size_t i = 0; i < img.pixels.size(); ++i) {
            const u32 p = img.pixels[i];
            r.bgra[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
        }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = img.width;
        bi.bmiHeader.biHeight = -img.height;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HWND hwnd = reinterpret_cast<HWND>(r.handle);
        HDC dc = GetDC(hwnd);
        SetDIBitsToDevice(dc, 0, 0, DWORD(img.width), DWORD(img.height), 0, 0, 0, UINT(img.height), r.bgra.data(), &bi,
                          DIB_RGB_COLORS);
        ReleaseDC(hwnd, dc);
        r.impl->syncPopups(r);
        return true;
    }
#endif
    default: w.rasterize(damage); r.impl->syncPopups(r); return true;
    }
}

void NativePlatform::Impl::destroy(void* user) {
    Rec* r = static_cast<Rec*>(user);
    Impl& m = *r->impl;
    detail::vkWinDestroy(r->vk);
    r->vk = nullptr;
    for (Rec::Popup& p : r->popups) m.destroyPopupOs(p);
    r->popups.clear();
    if (r->win) r->win->ui().setOsPopups(false);
    m.destroyOsWindow(*r);
    std::erase(m.recs, r);
    delete r;
}

#if defined(_WIN32)
// ---- Win32 -----------------------------------------------------------------------------------------------------------
namespace {
constexpr const wchar_t* kClass = L"FuseAppWindow";
Key mapVk(WPARAM vk, bool& ascii) {
    ascii = false;
    switch (vk) {
    case VK_TAB: return Key::Tab;
    case VK_RETURN: return Key::Enter;
    case VK_ESCAPE: return Key::Escape;
    case VK_SPACE: return Key::Space;
    case VK_BACK: return Key::Backspace;
    case VK_DELETE: return Key::Delete;
    case VK_LEFT: return Key::Left;
    case VK_RIGHT: return Key::Right;
    case VK_UP: return Key::Up;
    case VK_DOWN: return Key::Down;
    case VK_HOME: return Key::Home;
    case VK_END: return Key::End;
    case VK_PRIOR: return Key::PageUp;
    case VK_NEXT: return Key::PageDown;
    case VK_F2: return Key::F2;
    case VK_F5: return Key::F5;
    case VK_F10: return Key::F10;
    case VK_F1: return Key::F1;
    case VK_F3: return Key::F3;
    case VK_F4: return Key::F4;
    case VK_F6: return Key::F6;
    case VK_F7: return Key::F7;
    case VK_F8: return Key::F8;
    case VK_F9: return Key::F9;
    case VK_F11: return Key::F11;
    case VK_F12: return Key::F12;
    case VK_OEM_4: return Key::BracketLeft;
    case VK_OEM_6: return Key::BracketRight;
    default: break;
    }
    Key k = Key::Other;
    ascii = vk < 0x80 && vk != 0x5B && vk != 0x5D && asciiKey(k, u32(vk)); // 0x5B / 0x5D are VK_LWIN / VK_APPS, not [ ]
    return k;
}
u8 winMods() {
    return u8(((GetKeyState(VK_SHIFT) & 0x8000) ? kModShift : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? kModCtrl : 0) |
              ((GetKeyState(VK_MENU) & 0x8000) ? kModAlt : 0) |
              (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) ? kModSuper : 0));
}
Vec2 lparamPos(LPARAM lp) { return {f32(short(LOWORD(lp))), f32(short(HIWORD(lp)))}; }

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    AppWindow& w = *r->win;
    UiContext& ui = w.ui();
    switch (msg) {
    case WM_CLOSE: w.requestClose(); return 0;
    case WM_ACTIVATEAPP: // another application took the pointer / focus: popups close (Qt::Popup)
        if (!wp && !r->popups.empty()) NativePlatform::Impl::closeAllPopups(*r);
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->mapped = true;
        w.requestRedraw();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (LOWORD(lp) && HIWORD(lp)) w.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        ui.setModifiers(u8(((wp & MK_SHIFT) ? kModShift : 0) | ((wp & MK_CONTROL) ? kModCtrl : 0) | ((GetKeyState(VK_MENU) < 0) ? kModAlt : 0)));
        break;
    default: break;
    }
    switch (msg) {
    case WM_MOUSEMOVE: ui.pointerMove(lparamPos(lp)); return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd); ui.pointerDown(lparamPos(lp), MouseButton::Left); return 0;
    case WM_RBUTTONDOWN: ui.pointerDown(lparamPos(lp), MouseButton::Right); return 0;
    case WM_MBUTTONDOWN: ui.pointerDown(lparamPos(lp), MouseButton::Middle); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); ui.pointerUp(lparamPos(lp)); return 0;
    case WM_RBUTTONUP:
    case WM_MBUTTONUP: ui.pointerUp(lparamPos(lp)); return 0;
    case WM_MOUSEWHEEL: {
        POINT p{short(LOWORD(lp)), short(HIWORD(lp))};
        ScreenToClient(hwnd, &p);
        ui.wheel({f32(p.x), f32(p.y)}, {0.f, f32(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA});
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool ascii = false;
        const Key k = mapVk(wp, ascii);
        const u8 md = winMods();
        if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
            if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ui.keyDown(k, md);
            else ui.keyUp(k, md);
        }
        break;
    }
    case WM_CHAR: {
        const wchar_t c = wchar_t(wp);
        if (c >= 0xD800 && c < 0xDC00) { r->highSurrogate = c; return 0; }
        u32 cp = c;
        if (c >= 0xDC00 && c < 0xE000 && r->highSurrogate) cp = 0x10000u + ((u32(r->highSurrogate) - 0xD800u) << 10) + (c - 0xDC00u);
        r->highSurrogate = 0;
        if (cp < 0x20 || cp == 0x7F || (winMods() & kModCtrl)) return 0;
        std::string s;
        appendUtf8(s, cp);
        ui.textInput(s);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
constexpr const wchar_t* kPopupClass = L"FuseAppPopup";
LRESULT CALLBACK popupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* r = reinterpret_cast<NativePlatform::Impl::Rec*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE; // the main window keeps keyboard focus (Qt popup behaviour)
    if (!r || !r->win) return DefWindowProcW(hwnd, msg, wp, lp);
    std::size_t idx = 0;
    if (!r->impl->findPopup(reinterpret_cast<unsigned long long>(hwnd), idx)) return DefWindowProcW(hwnd, msg, wp, lp);
    NativePlatform::Impl::Rec::Popup& p = r->popups[idx];
    UiContext& ui = r->win->ui();
    const f32 s = 1.f;
    const Vec2 lp2 = lparamPos(lp);
    const Vec2 pos{p.rect.x + lp2.x / s, p.rect.y + lp2.y / s};
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        r->impl->blitPopupOs(p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: ui.pointerMove(pos); r->win->requestRedraw(); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
        if (lp2.x < 0.f || lp2.y < 0.f || lp2.x >= p.screen.w || lp2.y >= p.screen.h) { // captured click elsewhere
            NativePlatform::Impl::closeAllPopups(*r);
            return 0;
        }
        if (msg == WM_LBUTTONDOWN) ui.pointerDown(pos, MouseButton::Left);
        else ui.pointerDown(pos, MouseButton::Right);
        r->win->requestRedraw();
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: ui.pointerUp(pos); r->win->requestRedraw(); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    POINT o{0, 0};
    if (!ClientToScreen(hwnd, &o)) return false;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    ox = o.x;
    oy = o.y;
    work = {f32(mi.rcWork.left), f32(mi.rcWork.top), f32(mi.rcWork.right - mi.rcWork.left), f32(mi.rcWork.bottom - mi.rcWork.top)};
    return true;
}
void NativePlatform::Impl::grabPointer(Rec&, bool) {} // Win32: WM_ACTIVATEAPP closes popups instead of a grab
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kPopupClass, L"", WS_POPUP, int(p.screen.x),
                                int(p.screen.y), int(p.screen.w), int(p.screen.h), reinterpret_cast<HWND>(r.handle), nullptr, hinst, nullptr);
    if (!hwnd) return false;
    p.handle = reinterpret_cast<unsigned long long>(hwnd);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    SetWindowPos(reinterpret_cast<HWND>(p.handle), nullptr, int(p.screen.x), int(p.screen.y), int(p.screen.w), int(p.screen.h),
                 SWP_NOACTIVATE | SWP_NOZORDER);
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) {
        const u32 q = p.img.pixels[i];
        p.staged[i] = (q & 0xFF00FF00u) | ((q & 0xFFu) << 16) | ((q >> 16) & 0xFFu);
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = p.img.width;
    bi.bmiHeader.biHeight = -p.img.height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HWND hwnd = reinterpret_cast<HWND>(p.handle);
    HDC dc = GetDC(hwnd);
    SetDIBitsToDevice(dc, 0, 0, DWORD(p.img.width), DWORD(p.img.height), 0, 0, 0, UINT(p.img.height), p.staged.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(hwnd, dc);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    HWND hwnd = reinterpret_cast<HWND>(h);
    const LPARAM lp = MAKELPARAM(int(at.x), int(at.y));
    return PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp) && PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp) &&
           PostMessageW(hwnd, WM_LBUTTONUP, 0, lp);
}

bool NativePlatform::Impl::createOsWindow(Rec& r) {
    const Size sz = r.win->size();
    RECT rc{0, 0, LONG(sz.w), LONG(sz.h)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const std::string& t = r.win->title();
    std::wstring wt(std::size_t(MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), nullptr, 0)), L'\0');
    if (!wt.empty()) MultiByteToWideChar(CP_UTF8, 0, t.data(), int(t.size()), wt.data(), int(wt.size()));
    HWND hwnd = CreateWindowExW(0, kClass, wt.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) {
        error = "CreateWindowExW failed";
        return false;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&r));
    r.handle = reinterpret_cast<unsigned long long>(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (!r.handle) return;
    HWND hwnd = reinterpret_cast<HWND>(r.handle);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    DestroyWindow(hwnd);
    r.handle = 0;
}
bool NativePlatform::Impl::pump(void* user, Application&, f32 wait) {
    (void)user;
    MSG msg;
    if (wait > 0.f && !PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
        const DWORD ms = std::isinf(wait) ? INFINITE : DWORD(std::ceil(double(wait) * 1000.0));
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}
static bool captureHwnd(HWND hwnd, Image& out) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    HDC wdc = GetDC(hwnd);
    HDC mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    const bool okBlt = BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY) != 0;
    GdiFlush();
    if (okBlt && bits) {
        out.resize(w, h);
        const u32* src = static_cast<const u32*>(bits);
        for (std::size_t i = 0; i < out.pixels.size(); ++i) {
            const u32 p = src[i];
            out.pixels[i] = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
        }
    }
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(hwnd, wdc);
    return okBlt;
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    // GDI cannot read a Vulkan-presented client area (DWM flip model on Windows; a separate X drawable under Wine), so
    // a Vulkan window reports the frame it presented; the GDI fallback is read from the window DC.
    if (r.vk) return detail::vkWinReadback(r.vk, out);
    return captureHwnd(reinterpret_cast<HWND>(r.handle), out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureHwnd(reinterpret_cast<HWND>(p.handle), out);
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    m->hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &wndProc;
    wc.hInstance = m->hinst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc); // fails harmlessly when a second NativePlatform registers again
    WNDCLASSEXW pc = wc;
    pc.style = CS_SAVEBITS | CS_DROPSHADOW;
    pc.lpfnWndProc = &popupProc;
    pc.lpszClassName = kPopupClass;
    RegisterClassExW(&pc);
    app.setEventSource({m.get(), &Impl::pump});
}
bool NativePlatform::available() const { return true; }
void* NativePlatform::nativeDisplay() const { return m->hinst; }

#else
// ---- X11 platform ------------------------------------------------------------------------------------------------
bool NativePlatform::Impl::createOsWindow(Rec& r) {
    const Size sz = r.win->size();
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), 0, 0, unsigned(sz.w), unsigned(sz.h), 0, 0, 0);
    if (!w) {
        error = "XCreateSimpleWindow failed";
        return false;
    }
    x.StoreName(dpy, w, r.win->title().c_str());
    x.SelectInput(dpy, w, x11::kMask);
    x.SetWMProtocols(dpy, w, &wmDelete, 1);
    x.MapWindow(dpy, w);
    x.Flush(dpy);
    r.handle = w;
    return true;
}
void NativePlatform::Impl::destroyOsWindow(Rec& r) {
    if (!r.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(r.handle));
    x.Flush(dpy);
    r.handle = 0;
}
bool NativePlatform::Impl::pump(void* user, Application& app, f32 wait) {
    Impl& m = *static_cast<Impl*>(user);
    if (!m.dpy) return m.previous.pump ? m.previous.pump(m.previous.user, app, wait) : true;
    auto& x = m.x;
    if (wait > 0.f && !x.Pending(m.dpy)) {
        pollfd p{x.ConnectionNumber(m.dpy), POLLIN, 0};
        const int ms = std::isinf(wait) ? -1 : int(std::min(std::ceil(double(wait) * 1000.0), 2.0e9));
        ::poll(&p, 1, ms);
    }
    while (x.Pending(m.dpy)) {
        x11::Event ev{};
        x.NextEvent(m.dpy, &ev);
        std::size_t pi = 0;
        if (Rec* pr = m.findPopup(ev.any.window, pi)) { // a popup window: input in context coordinates of its owner
            Rec::Popup& p = pr->popups[pi];
            UiContext& ui = pr->win->ui();
            const f32 s = 1.f;
            const Vec2 pos{p.rect.x + f32(ev.input.x) / s, p.rect.y + f32(ev.input.y) / s};
            if (ev.type == x11::Expose) m.blitPopupOs(p);
            else if (ev.type == x11::MotionNotify) { ui.setModifiers(x11::mods(ev.input.state)); ui.pointerMove(pos); }
            else if (ev.type == x11::ButtonPress && (ev.input.x < 0 || ev.input.y < 0 || f32(ev.input.x) >= p.screen.w ||
                                                     f32(ev.input.y) >= p.screen.h)) {
                closeAllPopups(*pr); // grabbed click outside every window of the app
                continue;
            } else if (ev.type == x11::ButtonPress) {
                ui.setModifiers(x11::mods(ev.input.state));
                if (ev.input.detail == 4 || ev.input.detail == 5) ui.wheel(pos, {0.f, ev.input.detail == 4 ? 1.f : -1.f});
                else ui.pointerDown(pos, ev.input.detail == 3 ? MouseButton::Right : MouseButton::Left);
            } else if (ev.type == x11::ButtonRelease && ev.input.detail <= 3) ui.pointerUp(pos);
            pr->win->requestRedraw();
            continue;
        }
        Rec* r = m.find(ev.any.window);
        if (ev.type == x11::ConfigureNotify) r = m.find(ev.configure.window);
        if (!r || !r->win) continue;
        AppWindow& w = *r->win;
        UiContext& ui = w.ui();
        const x11::InputEv& in = ev.input;
        const Vec2 pos{f32(in.x), f32(in.y)};
        switch (ev.type) {
        case x11::ClientMessage:
            if (ev.client.message_type == m.wmProtocols && x11::Atom(ev.client.l[0]) == m.wmDelete) w.requestClose();
            break;
        case x11::MapNotify: r->mapped = true; w.requestRedraw(); break;
        case x11::Expose: r->mapped = true; w.requestRedraw(); break;
        case x11::ConfigureNotify: {
            const Size s = w.size();
            if (ev.configure.width > 0 && ev.configure.height > 0 &&
                (u32(ev.configure.width) != u32(s.w) || u32(ev.configure.height) != u32(s.h)))
                w.resize(u32(ev.configure.width), u32(ev.configure.height));
            break;
        }
        case x11::MotionNotify: ui.setModifiers(x11::mods(in.state)); ui.pointerMove(pos); break;
        case x11::ButtonPress:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail == 4 || in.detail == 5) ui.wheel(pos, {0.f, in.detail == 4 ? 1.f : -1.f});
            else if (in.detail == 6 || in.detail == 7) ui.wheel(pos, {in.detail == 6 ? -1.f : 1.f, 0.f});
            else ui.pointerDown(pos, in.detail == 3 ? MouseButton::Right : in.detail == 2 ? MouseButton::Middle : MouseButton::Left);
            break;
        case x11::ButtonRelease:
            ui.setModifiers(x11::mods(in.state));
            if (in.detail <= 3) ui.pointerUp(pos);
            break;
        case x11::KeyPress:
        case x11::KeyRelease: {
            char buf[32];
            x11::KeySym sym = 0;
            x11::InputEv copy = in;
            const int n = x.LookupString(&copy, buf, int(sizeof buf), &sym, nullptr);
            bool ascii = false;
            const Key k = x11::mapKey(sym, ascii);
            const u8 md = x11::mods(in.state);
            if (k != Key::Other) { // printable keys too: viewport WASD / 1-4; text arrives separately
                if (ev.type == x11::KeyPress) ui.keyDown(k, md);
                else ui.keyUp(k, md);
            }
            if (ev.type == x11::KeyPress && n > 0 && !(md & (kModCtrl | kModAlt))) {
                std::string s; // XLookupString yields Latin-1
                for (int i = 0; i < n; ++i)
                    if (u8(buf[i]) >= 0x20 && u8(buf[i]) != 0x7F) appendUtf8(s, u8(buf[i]));
                if (!s.empty()) ui.textInput(s);
            }
            break;
        }
        default: break;
        }
    }
    return true;
}
bool NativePlatform::Impl::screenInfo(const Rec& r, int& ox, int& oy, Rect& work) const {
    const int scr = x.DefaultScreen(dpy);
    x11::Window child = 0;
    if (!x.TranslateCoordinates(dpy, x11::Window(r.handle), x.RootWindow(dpy, scr), 0, 0, &ox, &oy, &child)) return false;
    work = {0.f, 0.f, f32(x.DisplayWidth(dpy, scr)), f32(x.DisplayHeight(dpy, scr))};
    return true;
}
bool NativePlatform::Impl::createPopupOs(Rec& r, Rec::Popup& p) {
    (void)r;
    const int scr = x.DefaultScreen(dpy);
    const x11::Window w = x.CreateSimpleWindow(dpy, x.RootWindow(dpy, scr), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w),
                                               unsigned(p.screen.h), 0, 0, 0);
    if (!w) return false;
    x11::SetWindowAttributes a{};
    a.override_redirect = 1; // no WM frame / focus: a menu window (Qt::Popup)
    a.save_under = 1;
    x.ChangeWindowAttributes(dpy, w, x11::kCWOverrideRedirect | x11::kCWSaveUnder, &a);
    const x11::Atom type = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE", 0), menu = x.InternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", 0);
    x.ChangeProperty(dpy, w, type, 4 /*XA_ATOM*/, 32, 0, reinterpret_cast<const unsigned char*>(&menu), 1);
    x.SelectInput(dpy, w, x11::kPopupMask);
    x.MapWindow(dpy, w);
    p.handle = w;
    return true;
}
void NativePlatform::Impl::grabPointer(Rec& r, bool on) {
    if (!on) { x.UngrabPointer(dpy, 0); x.Flush(dpy); return; }
    if (r.popups.empty() || !r.popups[0].handle) return;
    x.Sync(dpy, 0); // the override-redirect window must be viewable before it can grab
    // owner_events: the app's own windows (main + popups) keep their events; everything else reports to the popup
    x.GrabPointer(dpy, x11::Window(r.popups[0].handle), 1, (1u << 2) | (1u << 3) | (1u << 6), 1, 1, 0, 0, 0);
    x.Flush(dpy);
}
void NativePlatform::Impl::movePopupOs(Rec::Popup& p) {
    x.MoveResizeWindow(dpy, x11::Window(p.handle), int(p.screen.x), int(p.screen.y), unsigned(p.screen.w), unsigned(p.screen.h));
}
void NativePlatform::Impl::destroyPopupOs(Rec::Popup& p) {
    if (!p.handle || !dpy) return;
    x.DestroyWindow(dpy, x11::Window(p.handle));
    x.Flush(dpy);
    p.handle = 0;
}
void NativePlatform::Impl::blitPopupOs(Rec::Popup& p) {
    if (!p.handle || p.img.pixels.empty()) return;
    const int scr = x.DefaultScreen(dpy);
    if (x.DefaultDepth(dpy, scr) < 24) return;
    p.staged.resize(p.img.pixels.size());
    for (std::size_t i = 0; i < p.img.pixels.size(); ++i) { // 0xAABBGGRR -> TrueColor 0x00RRGGBB
        const u32 q = p.img.pixels[i];
        p.staged[i] = ((q & 0xFFu) << 16) | (q & 0xFF00u) | ((q >> 16) & 0xFFu);
    }
    x11::Image* img = x.CreateImage(dpy, x.DefaultVisual(dpy, scr), unsigned(x.DefaultDepth(dpy, scr)), 2 /*ZPixmap*/, 0,
                                    reinterpret_cast<char*>(p.staged.data()), unsigned(p.img.width), unsigned(p.img.height), 32, 0);
    if (!img) return;
    x.PutImage(dpy, x11::Window(p.handle), x.DefaultGC(dpy, scr), img, 0, 0, 0, 0, unsigned(p.img.width), unsigned(p.img.height));
    img->data = nullptr; // ours (XDestroyImage would free it)
    img->f.destroy(img);
    x.Flush(dpy);
}
bool NativePlatform::Impl::postClickOs(unsigned long long h, Vec2 at) {
    for (int type : {int(x11::ButtonPress), int(x11::ButtonRelease)}) {
        x11::Event ev{};
        ev.input.type = type;
        ev.input.display = dpy;
        ev.input.window = x11::Window(h);
        ev.input.root = x.RootWindow(dpy, x.DefaultScreen(dpy));
        ev.input.x = int(at.x);
        ev.input.y = int(at.y);
        ev.input.detail = 1;
        ev.input.same_screen = 1;
        if (!x.SendEvent(dpy, x11::Window(h), 0, 0, &ev)) return false;
    }
    x.Flush(dpy);
    return true;
}
bool NativePlatform::Impl::captureOs(const Rec& r, Image& out) {
    const Size sz = r.win->size();
    if (captureX(x11::Window(r.handle), unsigned(sz.w), unsigned(sz.h), out)) return true;
    // window not fully on screen (GetImage: BadMatch) or not mapped: a Vulkan window reports the frame it presented
    return r.vk && detail::vkWinReadback(r.vk, out);
}
bool NativePlatform::Impl::capturePopupOs(const Rec::Popup& p, Image& out) {
    return p.handle && captureX(x11::Window(p.handle), unsigned(p.screen.w), unsigned(p.screen.h), out);
}
bool NativePlatform::Impl::captureX(unsigned long win, unsigned w, unsigned h, Image& out) {
    x.Sync(dpy, 0);
    x11::Image* img = x.GetImage(dpy, win, 0, 0, w, h, ~0ul, 2 /*ZPixmap*/);
    if (!img) return false;
    bool good = img->bits_per_pixel == 32;
    if (good) {
        out.resize(img->width, img->height);
        for (int y = 0; y < img->height; ++y) {
            const u32* row = reinterpret_cast<const u32*>(img->data + std::ptrdiff_t(y) * img->bytes_per_line);
            for (int i = 0; i < img->width; ++i) {
                const u32 p = row[i]; // 0x00RRGGBB (TrueColor 24/32)
                out.pixels[std::size_t(y) * std::size_t(img->width) + std::size_t(i)] =
                    0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
            }
        }
    }
    img->f.destroy(img);
    return good;
}

NativePlatform::NativePlatform(Application& app, const NativeOptions& o) : m(std::make_unique<Impl>(app, o)) {
    const char* disp = std::getenv("DISPLAY");
    if (disp && *disp && m->x.load()) {
        m->dpy = m->x.OpenDisplay(nullptr);
        if (m->dpy) {
            m->x.SetErrorHandler(&x11::ignoreErrors);
            m->wmProtocols = m->x.InternAtom(m->dpy, "WM_PROTOCOLS", 0);
            m->wmDelete = m->x.InternAtom(m->dpy, "WM_DELETE_WINDOW", 0);
        } else {
            m->error = "cannot open X display";
        }
    } else {
        m->error = disp && *disp ? "libX11.so.6 not found" : "no DISPLAY";
    }
    if (m->dpy) app.setEventSource({m.get(), &Impl::pump});
}
bool NativePlatform::available() const { return m->dpy != nullptr; }
void* NativePlatform::nativeDisplay() const { return m->dpy; }
#endif

NativePlatform::~NativePlatform() {
    // detach every shown window (restores its headless presenter) before the display / instance go away
    while (!m->recs.empty()) m->recs.back()->win->setPresenter({});
    detail::vkCtxDestroy(m->vk);
#if defined(_WIN32)
    m->app.setEventSource({});
#else
    if (m->dpy) {
        m->x.CloseDisplay(m->dpy);
        m->app.setEventSource({});
    }
#endif
}

PresenterKind NativePlatform::show(AppWindow& w) {
    if (const Impl::Rec* existing = m->find(w)) return existing->kind;
    if (!available()) return PresenterKind::Headless;
    auto* r = new Impl::Rec{};
    r->impl = m.get();
    r->win = &w;
    m->recs.push_back(r);
    if (!m->createOsWindow(*r)) {
        std::erase(m->recs, r);
        delete r;
        return PresenterKind::Headless;
    }
    if (detail::VkCtx* c = m->vkContext()) {
        SurfaceSource s;
#if defined(_WIN32)
        s.kind = SurfaceSource::Win32;
        s.hinstance = m->hinst;
        s.hwnd = reinterpret_cast<void*>(r->handle);
#else
        s.kind = SurfaceSource::Xlib;
        s.display = m->dpy;
        s.xwindow = static_cast<unsigned long>(r->handle);
#endif
        r->vk = detail::vkWinCreate(c, s, m->opts, m->error);
    }
#if defined(_WIN32)
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Software;
#else
    r->kind = r->vk ? PresenterKind::Vulkan : PresenterKind::Headless;
#endif
    if (m->opts.osPopups) w.ui().setOsPopups(true); // screen bounds arrive with the first present
    w.setPresenter({r, &Impl::present, &Impl::destroy});
    return r->kind;
}

PresenterKind NativePlatform::kind(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->kind : PresenterKind::Headless;
}
std::string_view NativePlatform::gpuName() const { return detail::vkCtxDeviceName(m->vk); }
bool NativePlatform::gpuDraws(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinGpuDraws(r->vk);
}
std::string_view NativePlatform::lastError() const { return m->error; }
bool NativePlatform::setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex) {
    const Impl::Rec* r = m->find(w);
    return r && detail::vkWinSetExternal(r->vk, slot, tex);
}
bool NativePlatform::capture(const AppWindow& w, Image& out) {
    const Impl::Rec* r = m->find(w);
    return r && r->handle && m->captureOs(*r, out);
}
std::size_t NativePlatform::popupCount(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->popups.size() : 0;
}
unsigned long long NativePlatform::popupWindow(const AppWindow& w, std::size_t i) const {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].handle : 0;
}
Rect NativePlatform::popupScreenRect(const AppWindow& w, std::size_t i) const {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() ? r->popups[i].screen : Rect{};
}
bool NativePlatform::capturePopup(const AppWindow& w, std::size_t i, Image& out) {
    const Impl::Rec* r = m->find(w);
    return r && i < r->popups.size() && m->capturePopupOs(r->popups[i], out);
}
Vec2 NativePlatform::screenOrigin(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    int ox = 0, oy = 0;
    Rect work;
    if (!r || !r->handle || !m->screenInfo(*r, ox, oy, work)) return {};
    return {f32(ox), f32(oy)};
}
bool NativePlatform::postClick(unsigned long long nativeWin, Vec2 at) { return nativeWin && m->postClickOs(nativeWin, at); }
unsigned long long NativePlatform::nativeWindow(const AppWindow& w) const {
    const Impl::Rec* r = m->find(w);
    return r ? r->handle : 0;
}

namespace {
// fuse::ui::Image pixels are 0xAABBGGRR (R in the low byte); OS icons want 0xAARRGGBB
u32 toArgb(u32 p) { return (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu); }
} // namespace

#if defined(_WIN32)
namespace {
HICON makeIcon(const Image& img) {
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width;
    bi.bV5Height = -img.height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000u;
    bi.bV5GreenMask = 0x0000FF00u;
    bi.bV5BlueMask = 0x000000FFu;
    bi.bV5AlphaMask = 0xFF000000u;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color || !bits) return nullptr;
    u32* dst = static_cast<u32*>(bits);
    for (std::size_t i = 0; i < img.pixels.size(); ++i) dst[i] = toArgb(img.pixels[i]);
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}
const Image* closest(std::span<const Image> sizes, int want) {
    const Image* best = nullptr;
    for (const Image& i : sizes)
        if (i.width > 0 && (!best || std::abs(i.width - want) < std::abs(best->width - want))) best = &i;
    return best;
}
int iconWidth(HICON icon) {
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return 0;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return bm.bmWidth;
}
} // namespace

bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    bool ok = false;
    for (const auto [which, metric] : {std::pair{ICON_BIG, SM_CXICON}, std::pair{ICON_SMALL, SM_CXSMICON}}) {
        const Image* img = closest(sizes, GetSystemMetrics(metric));
        if (!img) continue;
        if (HICON icon = makeIcon(*img)) {
            if (HICON old = reinterpret_cast<HICON>(SendMessageW(hwnd, WM_SETICON, WPARAM(which), LPARAM(icon)))) DestroyIcon(old);
            ok = true;
        }
    }
    return ok;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return false;
    RECT rc{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    return SetWindowPos(reinterpret_cast<HWND>(r->handle), nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle) return out;
    HWND hwnd = reinterpret_cast<HWND>(r->handle);
    for (WPARAM which : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL)})
        if (const int wpx = iconWidth(reinterpret_cast<HICON>(SendMessageW(hwnd, WM_GETICON, which, 0)))) out.push_back(wpx);
    return out;
}
#else
bool NativePlatform::setIcon(const AppWindow& w, std::span<const Image> sizes) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    std::vector<unsigned long> data; // format-32 X properties are arrays of C long
    for (const Image& img : sizes) {
        if (img.width <= 0 || img.height <= 0 || img.pixels.size() != std::size_t(img.width) * std::size_t(img.height)) continue;
        data.push_back(static_cast<unsigned long>(img.width));
        data.push_back(static_cast<unsigned long>(img.height));
        for (u32 p : img.pixels) data.push_back(toArgb(p));
    }
    if (data.empty()) return false;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    const x11::Atom cardinal = 6; // XA_CARDINAL
    m->x.ChangeProperty(m->dpy, x11::Window(r->handle), netIcon, cardinal, 32, 0 /*PropModeReplace*/,
                        reinterpret_cast<const unsigned char*>(data.data()), int(data.size()));
    m->x.Flush(m->dpy);
    return true;
}
bool NativePlatform::resizeWindow(const AppWindow& w, unsigned width, unsigned height) {
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return false;
    m->x.ResizeWindow(m->dpy, x11::Window(r->handle), width, height);
    m->x.Flush(m->dpy);
    return true;
}
std::vector<int> NativePlatform::iconSizes(const AppWindow& w) const {
    std::vector<int> out;
    const Impl::Rec* r = m->find(w);
    if (!r || !r->handle || !m->dpy) return out;
    const x11::Atom netIcon = m->x.InternAtom(m->dpy, "_NET_WM_ICON", 0);
    x11::Atom type = 0;
    int format = 0;
    unsigned long n = 0, after = 0;
    unsigned char* prop = nullptr;
    m->x.Sync(m->dpy, 0);
    if (m->x.GetWindowProperty(m->dpy, x11::Window(r->handle), netIcon, 0, 1L << 22, 0, 6, &type, &format, &n, &after, &prop) != 0 ||
        !prop)
        return out;
    const unsigned long* v = reinterpret_cast<const unsigned long*>(prop);
    for (unsigned long i = 0; i + 2 <= n;) {
        const unsigned long iw = v[i], ih = v[i + 1];
        if (!iw || !ih || i + 2 + iw * ih > n) break;
        out.push_back(int(iw));
        i += 2 + iw * ih;
    }
    m->x.Free(prop);
    return out;
}
#endif

} // namespace fuse::app
