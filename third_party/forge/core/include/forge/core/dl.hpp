#pragma once
#if defined(_WIN32)
#define FORGE_DLNAME(posix, win) win
#else
#define FORGE_DLNAME(posix, win) posix
#endif
// dlopen/dlsym/dlclose on every platform (Win32: UTF-8 names via forge::os::widen -> LoadLibraryW). New code: use forge::os::library. Include instead of <dlfcn.h>.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <forge/os/os.hpp>
#ifndef RTLD_NOW
#define RTLD_NOW 2
#define RTLD_LAZY 1
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#endif
inline void* dlopen(const char* name, int) { return name ? static_cast<void*>(LoadLibraryW(forge::os::widen(name).c_str())) : static_cast<void*>(GetModuleHandleA(nullptr)); }
inline void* dlsym(void* h, const char* s) { return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h), s)); }
inline int dlclose(void* h) { return FreeLibrary(static_cast<HMODULE>(h)) ? 0 : 1; }
inline const char* dlerror() { static thread_local char b[64]; wsprintfA(b, "Win32 error %lu", GetLastError()); return b; }
#else
#include <dlfcn.h>
#endif
