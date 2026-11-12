// fuse_app native backends: the Vulkan swapchain presenter shared by the X11 and Win32 window code.
#pragma once

#include "fuse/app/native.hpp"

#include <string>

namespace fuse::app::detail {

struct SurfaceSource {
    enum Kind { Xlib, Win32 } kind = Xlib;
    void* display = nullptr;        ///< X11 Display*
    unsigned long xwindow = 0;      ///< X11 Window
    void* hinstance = nullptr;      ///< Win32 HINSTANCE
    void* hwnd = nullptr;           ///< Win32 HWND
};

struct VkCtx;
struct VkWin;
/// Instance with the surface extension for `kind`; device chosen on the first window. Null when Vulkan is missing.
VkCtx* vkCtxCreate(SurfaceSource::Kind kind, std::string& err);
/// Wraps an existing instance + device (NativeOptions::adoptDevice); not destroyed by vkCtxDestroy.
VkCtx* vkCtxAdopt(SurfaceSource::Kind kind, const VulkanDeviceHandles& h, std::string& err);
void vkCtxDestroy(VkCtx* c);
bool vkWinSetExternal(VkWin* w, unsigned slot, const ExternalTexture& t);
const char* vkCtxDeviceName(const VkCtx* c);
VkWin* vkWinCreate(VkCtx* c, const SurfaceSource& s, const NativeOptions& o, std::string& err);
bool vkWinPresent(VkWin* w, AppWindow& win, fuse::ui::Rect damage);
bool vkWinGpuDraws(const VkWin* w);
void vkWinDestroy(VkWin* w);
/// Reads back the last presented frame (the canvas blitted into the swapchain).
bool vkWinReadback(VkWin* w, fuse::ui::Image& out);

} // namespace fuse::app::detail
