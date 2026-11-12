// fuse_app macOS backend: a plain C++ interface to the Cocoa code in native_cocoa.mm.
//
// RetDec addition (not upstream FUSE yet). native.cpp stays C++: <Cocoa/Cocoa.h> pulls in MacTypes.h, whose global
// Rect / Size / Point would collide with fuse::ui's names under native.cpp's `using namespace fuse::ui`, so all
// Objective-C lives in native_cocoa.mm behind this header.
//
// Windows are NSWindow + an NSView that draws the CPU raster (fuse::ui::Image pixels, 0xAABBGGRR) as a CGImage.
// Events come back through Callbacks in window-local, top-left-origin logical coordinates.
#pragma once

#include <cstdint>
#include <string>

namespace fuse::app::cocoa {

struct Callbacks {
    void* user = nullptr;
    void (*close)(void* user) = nullptr; ///< close button / Cmd+W: the app decides (AppWindow::userCloseRequest)
    void (*resize)(void* user, unsigned width, unsigned height) = nullptr;
    void (*expose)(void* user) = nullptr;
    void (*modifiers)(void* user, std::uint8_t mods) = nullptr;
    void (*pointerMove)(void* user, float x, float y) = nullptr;
    void (*pointerDown)(void* user, float x, float y, int button) = nullptr; ///< 0 left, 1 right, 2 middle
    void (*pointerUp)(void* user, float x, float y) = nullptr;
    void (*wheel)(void* user, float x, float y, float dx, float dy) = nullptr;
    void (*key)(void* user, int key, std::uint8_t mods, bool down) = nullptr; ///< key: a fuse::ui::Key value
    void (*text)(void* user, const char* utf8) = nullptr;
    void (*focus)(void* user, bool gained) = nullptr;
    /// File drops: whether the app takes them now (AppWindow::acceptsFileDrops), and the drop itself (UTF-8 paths,
    /// window-local top-left point).
    bool (*acceptsDrop)(void* user) = nullptr;
    void (*drop)(void* user, const char* const* paths, int count, float x, float y) = nullptr;
};

/// Sets up NSApplication (regular activation policy, an application menu with Quit). False without a window server.
bool init(const std::string& appName, std::string& error);
/// Opens a titled, resizable window with a content area of width x height logical points. Null on failure.
void* createWindow(const std::string& title, unsigned width, unsigned height, const Callbacks& cb);
void destroyWindow(void* window);
/// Shows `rgba` (fuse::ui::Image pixels, width x height) in the window; the view redraws on the next display cycle.
void blit(void* window, const std::uint32_t* rgba, int width, int height);
/// Reads back what blit() last showed (0xAABBGGRR). False when nothing was shown yet.
bool readback(void* window, std::uint32_t* rgba, int width, int height);
/// Processes events, waiting up to `waitSeconds` (infinity: until one arrives) for the first. False once the user
/// chose Quit from the application menu.
bool pump(float waitSeconds);
bool resize(void* window, unsigned width, unsigned height);
/// Content-area top-left in screen points (top-left origin) and the visible work area of its screen.
bool screenInfo(void* window, int& x, int& y, float& workX, float& workY, float& workW, float& workH);
/// Sets the application (Dock) icon from a square RGBA raster.
bool setIcon(const std::uint32_t* rgba, int width, int height);
int iconSize();

} // namespace fuse::app::cocoa
