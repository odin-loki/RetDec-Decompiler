// fuse_app native windows (plan UI5): real OS windows (X11 via a runtime-loaded libX11, Win32) with a Vulkan swapchain
// presenter. The UI is drawn either by the fuse_ui_vk compute kernel straight into a storage image (GpuDraw) or by the
// CPU rasteriser and uploaded (CpuUpload); either result is blitted into the swapchain image and presented. Without a
// Vulkan device the Win32 backend falls back to GDI (StretchDIBits); X11 then keeps the window but cannot draw it.
//
//   fuse::app::Application app{{.name = "hello"}};
//   fuse::app::NativePlatform native(app);          // installs the OS event source (blocks in poll / MsgWait)
//   auto& w = app.createWindow({.title = "Hello"});
//   native.show(w);                                 // OS window + presenter; headless when no display
//   return app.run();
//
// Declare NativePlatform after the Application (it is destroyed first and detaches its windows and event source).
#pragma once

#include "fuse/app/app.hpp"

#include <memory>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace fuse::app {

enum class PresenterKind : fuse::ui::u8 {
    Headless, ///< no OS window (no display / not shown); CPU raster into AppWindow::image()
    Vulkan,   ///< swapchain presenter
    Software, ///< GDI blit of the CPU raster (Win32 without Vulkan)
};
enum class VkDrawMode : fuse::ui::u8 {
    Auto,      ///< GpuDraw when the compute kernel is compiled in, else CpuUpload
    GpuDraw,   ///< fuse_ui_vk "ui.draw_list" kernel renders the draw list on the GPU
    CpuUpload, ///< CPU raster of the damaged region, uploaded and blitted
};
/// Raw Vulkan handles (VkInstance / VkPhysicalDevice / VkDevice / VkQueue). As NativeOptions::adoptDevice the
/// presenter runs on this device instead of creating its own (the instance needs VK_KHR_surface + the platform surface
/// extension, the device VK_KHR_swapchain, the queue graphics or compute + present): GPU images of the same device
/// (an engine frame) can then be drawn by the UI kernel without a copy (setExternalTexture). The presenter submits on
/// `queue` from the UI thread; the owner keeps the device alive past the NativePlatform and must not use the queue
/// from another thread concurrently.
struct VulkanDeviceHandles {
    void* instance = nullptr;
    void* physicalDevice = nullptr;
    void* device = nullptr;
    void* queue = nullptr;
    unsigned queueFamily = 0;
};
/// A sampled image of the presenter's device for fuse_ui_vk's external slots (fuse::ui::Image::externalSlot).
struct ExternalTexture {
    void* image = nullptr;  ///< VkImage (null: unbind)
    void* view = nullptr;   ///< VkImageView, sampled usage
    unsigned layout = 0;    ///< its VkImageLayout when the UI frame is submitted; restored after the UI pass
};
struct NativeOptions {
    VulkanDeviceHandles adoptDevice{}; ///< instance + device set: present on that device (see VulkanDeviceHandles)
    bool vulkan = true;            ///< false: never create a Vulkan presenter
    VkDrawMode drawMode = VkDrawMode::Auto;
    bool vsync = true;             ///< FIFO (true) or MAILBOX / IMMEDIATE when available
    /// Popup menus / combo lists / submenus as their own borderless OS windows (Qt-style; may extend past the main
    /// window, clamp to the screen). X11: override-redirect windows; Win32: WS_POPUP + WS_EX_NOACTIVATE owned windows.
    bool osPopups = false;
};

class NativePlatform {
public:
    explicit NativePlatform(Application& app, const NativeOptions& o = {});
    ~NativePlatform();
    NativePlatform(const NativePlatform&) = delete;
    NativePlatform& operator=(const NativePlatform&) = delete;

    /// True when an OS display connection is open (X11: $DISPLAY; Win32: always).
    bool available() const;
    /// Opens an OS window for `w` (title / size from its WindowDesc) and installs the best presenter.
    PresenterKind show(AppWindow& w);
    PresenterKind kind(const AppWindow& w) const;
    /// "lavapipe (LLVM ...)" etc. once a Vulkan presenter exists; empty otherwise.
    std::string_view gpuName() const;
    /// True when the GPU kernel (not the CPU upload path) draws the window.
    bool gpuDraws(const AppWindow& w) const;
    std::string_view lastError() const;
    /// Reads back what the OS shows in the window's client area (tests / screenshots of real windows).
    bool capture(const AppWindow& w, fuse::ui::Image& out);
    /// Native handles: X11 Display* / Window, Win32 HINSTANCE / HWND (0 when not shown).
    void* nativeDisplay() const;
    /// Binds `tex` to external slot `slot` (0..3) of the window's GPU draw kernel; images whose externalSlot == slot
    /// then show it. Re-set it whenever the image, view or its layout change (each engine frame). False when the
    /// window is not GPU drawn (the CPU pixels of the Image are shown instead).
    bool setExternalTexture(const AppWindow& w, unsigned slot, const ExternalTexture& tex);
    unsigned long long nativeWindow(const AppWindow& w) const;
    /// Sets the OS window icon from square RGBA rasters (e.g. 16 / 32 / 48 / 256). X11: _NET_WM_ICON with every size;
    /// Win32: WM_SETICON big (SM_CXICON) and small (SM_CXSMICON), the closest raster each. False when not shown.
    bool setIcon(const AppWindow& w, std::span<const fuse::ui::Image> sizes);
    /// Asks the OS to resize the window's client area (logical px); the resize then arrives like a user resize
    /// (ConfigureNotify / WM_SIZE -> AppWindow::resize). False when not shown.
    bool resizeWindow(const AppWindow& w, unsigned width, unsigned height);
    /// Read back from the OS: the icon sizes (widths) the window carries now (X11 every _NET_WM_ICON entry; Win32 the
    /// big + small WM_GETICON icons). Empty when unset / not shown.
    std::vector<int> iconSizes(const AppWindow& w) const;

    /// OS-window popups (NativeOptions::osPopups): how many popup windows `w` shows now, each one's native handle,
    /// its screen rect (physical px) and a read-back of its pixels.
    std::size_t popupCount(const AppWindow& w) const;
    unsigned long long popupWindow(const AppWindow& w, std::size_t i) const;
    fuse::ui::Rect popupScreenRect(const AppWindow& w, std::size_t i) const;
    bool capturePopup(const AppWindow& w, std::size_t i, fuse::ui::Image& out);
    /// The window's top-left on screen (client area).
    fuse::ui::Vec2 screenOrigin(const AppWindow& w) const;
    /// Posts a real left click to a native window (XSendEvent / PostMessage) at window-local `at`; it is then
    /// delivered by the normal event pump (tests drive popup windows this way).
    bool postClick(unsigned long long nativeWin, fuse::ui::Vec2 at);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

} // namespace fuse::app
