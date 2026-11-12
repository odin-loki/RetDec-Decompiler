#pragma once
// Shared helpers for the forge::os extension headers: small text-file readers (sysfs/procfs), CPU-list parsing,
// and late-bound Apple frameworks (CoreFoundation, IOKit, Security, objc) so nothing needs SDK headers or link flags.
#include <forge/os/os.hpp>
#include <mutex>
#include <thread>
#include <deque>
#include <map>
#include <memory>
#include <condition_variable>
#include <algorithm>

namespace forge::os::detail {
inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; return s.substr(i);
}
inline std::string slurp(const std::string& p) {   // small pseudo-files: read() until EOF, "" on failure
#if defined(FORGE_OS_WINDOWS)
    auto r = read_file(p); return r ? r.value : std::string();
#else
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) return {};
    std::string s; char b[4096]; for (;;) { auto n = ::read(fd, b, sizeof b); if (n <= 0) break; s.append(b, std::size_t(n)); }
    ::close(fd); return s;
#endif
}
inline long long slurp_ll(const std::string& p, long long dflt = -1) {
    auto s = trim(slurp(p)); if (s.empty()) return dflt; char* e = nullptr; long long v = std::strtoll(s.c_str(), &e, 0); return e == s.c_str() ? dflt : v;
}
// "0-3,8,10-11" -> {0,1,2,3,8,10,11}
inline std::vector<unsigned> parse_cpulist(const std::string& s) {
    std::vector<unsigned> v; std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] < '0' || s[i] > '9') { ++i; continue; }
        unsigned a = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') a = a * 10 + unsigned(s[i++] - '0');
        unsigned b = a;
        if (i < s.size() && s[i] == '-') { ++i; b = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') b = b * 10 + unsigned(s[i++] - '0'); }
        for (unsigned c = a; c <= b && c < 65536; ++c) v.push_back(c);
    }
    return v;
}
inline std::string format_size(std::uint64_t n) {
    if (n >= (1ull << 30) && !(n % (1ull << 30))) return std::to_string(n >> 30) + "G";
    if (n >= (1ull << 20) && !(n % (1ull << 20))) return std::to_string(n >> 20) + "M";
    if (n >= 1024 && !(n % 1024)) return std::to_string(n >> 10) + "K";
    return std::to_string(n);
}
#if defined(FORGE_OS_POSIX) && !defined(__linux__)
template <class T> inline bool sysctl_get(const char* name, T& out) { std::size_t n = sizeof(T); return ::sysctlbyname(name, &out, &n, nullptr, 0) == 0 && n <= sizeof(T); }
inline long long sysctl_ll(const char* name, long long dflt = -1) {
    unsigned char b[8] = {}; std::size_t n = sizeof b;
    if (::sysctlbyname(name, b, &n, nullptr, 0)) return dflt;
    if (n == 4) { std::int32_t v; std::memcpy(&v, b, 4); return v; }
    if (n == 8) { std::int64_t v; std::memcpy(&v, b, 8); return v; }
    return dflt;
}
inline std::string sysctl_str(const char* name) {
    std::size_t n = 0; if (::sysctlbyname(name, nullptr, &n, nullptr, 0) || !n) return {};
    std::string s(n, '\0'); if (::sysctlbyname(name, s.data(), &n, nullptr, 0)) return {}; s.resize(std::strlen(s.c_str())); return s;
}
#endif
#if defined(__ANDROID__)
extern "C" int __system_property_get(const char*, char*);
inline std::string android_prop(const char* k) { char b[92] = {}; __system_property_get(k, b); return b; }
#endif

#if defined(__APPLE__)
// ---- late-bound Apple frameworks (no SDK headers needed; all C ABI)
using CFTypeRef = const void*; using CFStringRef = const void*; using CFDictionaryRef = const void*; using CFAllocatorRef = const void*;
using CFIndex = long; using Boolean = unsigned char;
struct cf_api {
    void* h = nullptr;
    CFStringRef (*CFStringCreateWithCString)(CFAllocatorRef, const char*, std::uint32_t) = nullptr;
    Boolean (*CFStringGetCString)(CFStringRef, char*, CFIndex, std::uint32_t) = nullptr;
    void (*CFRelease)(CFTypeRef) = nullptr;
    unsigned long (*CFGetTypeID)(CFTypeRef) = nullptr;
    unsigned long (*CFStringGetTypeID)() = nullptr;
    unsigned long (*CFNumberGetTypeID)() = nullptr;
    unsigned long (*CFDataGetTypeID)() = nullptr;
    Boolean (*CFNumberGetValue)(CFTypeRef, int, void*) = nullptr;
    CFDictionaryRef (*CFDictionaryCreate)(CFAllocatorRef, const void**, const void**, CFIndex, const void*, const void*) = nullptr;
    const void* (*CFDictionaryGetValue)(CFDictionaryRef, const void*) = nullptr;
    CFTypeRef (*CFDataCreate)(CFAllocatorRef, const std::uint8_t*, CFIndex) = nullptr;
    const std::uint8_t* (*CFDataGetBytePtr)(CFTypeRef) = nullptr;
    CFIndex (*CFDataGetLength)(CFTypeRef) = nullptr;
    CFIndex (*CFArrayGetCount)(CFTypeRef) = nullptr;
    const void* (*CFArrayGetValueAtIndex)(CFTypeRef, CFIndex) = nullptr;
    CFTypeRef (*CFArrayCreate)(CFAllocatorRef, const void**, CFIndex, const void*) = nullptr;
    const void* kCFTypeDictionaryKeyCallBacks = nullptr; const void* kCFTypeDictionaryValueCallBacks = nullptr; const void* kCFTypeArrayCallBacks = nullptr;
    const void* kCFBooleanTrue = nullptr;
    bool ok = false;
    static cf_api& get() {
        static cf_api a = [] {
            cf_api c; c.h = ::dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
            if (!c.h) return c;
#define FORGE_CF(n) c.n = reinterpret_cast<decltype(c.n)>(::dlsym(c.h, #n))
            FORGE_CF(CFStringCreateWithCString); FORGE_CF(CFStringGetCString); FORGE_CF(CFRelease); FORGE_CF(CFGetTypeID); FORGE_CF(CFStringGetTypeID);
            FORGE_CF(CFNumberGetTypeID); FORGE_CF(CFDataGetTypeID); FORGE_CF(CFNumberGetValue); FORGE_CF(CFDictionaryCreate); FORGE_CF(CFDictionaryGetValue);
            FORGE_CF(CFDataCreate); FORGE_CF(CFDataGetBytePtr); FORGE_CF(CFDataGetLength); FORGE_CF(CFArrayGetCount); FORGE_CF(CFArrayGetValueAtIndex); FORGE_CF(CFArrayCreate);
#undef FORGE_CF
            c.kCFTypeDictionaryKeyCallBacks = ::dlsym(c.h, "kCFTypeDictionaryKeyCallBacks"); c.kCFTypeDictionaryValueCallBacks = ::dlsym(c.h, "kCFTypeDictionaryValueCallBacks");
            c.kCFTypeArrayCallBacks = ::dlsym(c.h, "kCFTypeArrayCallBacks");
            if (auto p = static_cast<const void* const*>(::dlsym(c.h, "kCFBooleanTrue"))) c.kCFBooleanTrue = *p;
            c.ok = c.CFStringCreateWithCString && c.CFRelease && c.CFDictionaryCreate && c.kCFTypeDictionaryKeyCallBacks;
            return c;
        }();
        return a;
    }
    CFStringRef str(const char* s) const { return CFStringCreateWithCString(nullptr, s, 0x08000100 /*UTF8*/); }
    std::string to_string(CFTypeRef s) const {
        if (!s || CFGetTypeID(s) != CFStringGetTypeID()) return {};
        char b[1024] = {}; return CFStringGetCString(s, b, sizeof b, 0x08000100) ? b : "";
    }
    long long to_ll(CFTypeRef n, long long dflt = -1) const {
        if (!n || CFGetTypeID(n) != CFNumberGetTypeID()) return dflt; long long v = 0; return CFNumberGetValue(n, 4 /*kCFNumberSInt64Type*/, &v) ? v : dflt;
    }
};
// Global CFStringRef constants exported by a framework (kSecClass etc.)
inline const void* framework_const(void* h, const char* sym) { auto p = static_cast<const void* const*>(h ? ::dlsym(h, sym) : nullptr); return p ? *p : nullptr; }
struct objc_api {
    void* h = nullptr; void* (*getClass)(const char*) = nullptr; void* (*sel)(const char*) = nullptr; void* msg = nullptr;
    static objc_api& get() {
        static objc_api a = [] { objc_api o; o.h = ::dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY); if (!o.h) return o;
            o.getClass = reinterpret_cast<void* (*)(const char*)>(::dlsym(o.h, "objc_getClass"));
            o.sel = reinterpret_cast<void* (*)(const char*)>(::dlsym(o.h, "sel_registerName")); o.msg = ::dlsym(o.h, "objc_msgSend"); return o; }();
        return a;
    }
    bool ok() const { return getClass && sel && msg; }
    template <class R = void*, class... A> R send(void* obj, const char* s, A... a) const {
        return reinterpret_cast<R (*)(void*, void*, A...)>(msg)(obj, sel(s), a...);
    }
    void* cls(const char* n) const { return getClass ? getClass(n) : nullptr; }
};
#endif
} // namespace forge::os::detail
