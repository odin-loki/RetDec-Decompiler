#pragma once
// Background execution: keep long work (a model download, a long generation) alive when the app is backgrounded
// or the machine wants to sleep.
//   iOS: UIApplication beginBackgroundTaskWithName:expirationHandler: / endBackgroundTask: (objc runtime). The
//   expiration handler (a hand-built global block) ends the task, so iOS does not kill the app when time runs out;
//   kind() is then "" and active() drops.
//   Android: there is no NDK API — the app's JNI glue installs hooks (startForegroundService with a notification,
//   or WorkManager) via background::set_hooks(); without hooks begin() returns unsupported.
//   Windows: a power request per token (PowerCreateRequest + PowerSetRequest(SystemRequired), cleared on end(),
//   so end() works from any thread).  macOS: IOPMAssertion (PreventUserIdleSystemSleep).
//   Linux: a logind "sleep:idle" block inhibitor over D-Bus (libsystemd's sd-bus loaded at run time), held until
//   end(); without logind (containers, non-systemd) nothing idle-suspends the machine and kind is "not_required".
//   FreeBSD: no system idle-suspend policy exists (suspend is explicit: acpiconf/zzz), so kind is "not_required".
#include <forge/os/detail.hpp>

namespace forge::os {
class background {
public:
    struct hooks {   // Android JNI glue
        std::function<bool(const std::string& name, const std::string& title, const std::string& text)> begin;
        std::function<void(const std::string& name)> end;
    };
    static void set_hooks(hooks h) { std::lock_guard<std::mutex> g(m_()); hooks_() = std::move(h); }
    // Returns a token (>0) to pass to end(); kind() reports what the OS actually did.
    static result<std::uint64_t> begin(const std::string& name, const std::string& title = "FORGE", const std::string& text = "Working…") {
        std::lock_guard<std::mutex> g(m_()); std::uint64_t id = ++next_();
        task t; t.name = name;
#if defined(FORGE_OS_IOS)
        (void)title; (void)text;
        auto& o = detail::objc_api::get(); void* app = o.ok() ? o.send(o.cls("UIApplication"), "sharedApplication") : nullptr;
        if (!app) return {0, unsupported("UIApplication (not running inside an app)")};
        static void* isa = ::dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock"); if (!isa) return {0, unsupported("blocks runtime")};
        static const block_desc_t desc{0, sizeof(block_t)};
        auto* blk = new block_t{isa, 1 << 28 /*BLOCK_IS_GLOBAL*/, 0, &expired_, &desc, id};   // never freed: UIKit may hold it
        auto& cf = detail::cf_api::get(); auto nm = cf.str(name.c_str());
        t.native = o.send<std::uintptr_t, const void*, void*>(app, "beginBackgroundTaskWithName:expirationHandler:", nm, blk); cf.CFRelease(nm);
        if (t.native == 0 /*UIBackgroundTaskInvalid*/) return {0, {1, "beginBackgroundTask refused"}};
        t.kind = "ios-background-task";
#elif defined(__ANDROID__)
        if (!hooks_().begin) return {0, unsupported("android foreground service (install background::set_hooks from JNI)")};
        if (!hooks_().begin(name, title, text)) return {0, {1, "foreground service start refused"}};
        t.kind = "android-foreground-service";
#elif defined(FORGE_OS_WINDOWS)
        (void)title; (void)text;
        auto& p = power_api_::get(); if (!p.ok()) return {0, unsupported("PowerCreateRequest")};
        std::wstring why = widen(name.empty() ? std::string("forge") : name);
        power_api_::reason_t rc{}; rc.version = 0; rc.flags = 0x1 /*POWER_REQUEST_CONTEXT_SIMPLE_STRING*/; rc.simple = why.data();
        HANDLE h = p.create(&rc); if (h == INVALID_HANDLE_VALUE || !h) return {0, last_error("PowerCreateRequest")};
        if (!p.set(h, 1 /*PowerRequestSystemRequired*/)) { auto e = last_error("PowerSetRequest"); CloseHandle(h); return {0, e}; }
        t.native = reinterpret_cast<std::uintptr_t>(h); t.kind = "power-request";
#elif defined(FORGE_OS_MACOS)
        (void)title; (void)text;
        if (void* io = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY)) {
            auto create = reinterpret_cast<int (*)(const void*, std::uint32_t, const void*, std::uint32_t*)>(::dlsym(io, "IOPMAssertionCreateWithName"));
            auto& cf = detail::cf_api::get();
            if (create && cf.ok) { auto ty = cf.str("PreventUserIdleSystemSleep"), nm = cf.str(name.c_str()); std::uint32_t aid = 0;
                if (create(ty, 255, nm, &aid) == 0) { t.native = aid; t.kind = "iopm-assertion"; } cf.CFRelease(ty); cf.CFRelease(nm); }
        }
        if (t.kind.empty()) t.kind = "not_required";
#elif defined(FORGE_OS_LINUX)
        (void)title; (void)text;
        auto fd = logind_inhibit_(name.empty() ? std::string("forge") : name);
        if (!fd) return {0, fd.err};
        if (fd.value >= 0) { t.native = std::uintptr_t(fd.value); t.kind = "logind-inhibit"; } else t.kind = "not_required";
#else
        (void)title; (void)text; t.kind = "not_required";
#endif
        tasks_()[id] = t; return {id, {}};
    }
    static status end(std::uint64_t token) {
        std::lock_guard<std::mutex> g(m_()); auto it = tasks_().find(token); if (it == tasks_().end()) return {{}, {22, "background: unknown token"}};
        auto t = it->second; tasks_().erase(it);
        release_(t);
        return {};
    }
    static std::string kind(std::uint64_t token) { std::lock_guard<std::mutex> g(m_()); auto it = tasks_().find(token); return it == tasks_().end() ? "" : it->second.kind; }
    static std::size_t active() { std::lock_guard<std::mutex> g(m_()); return tasks_().size(); }
private:
    struct task { std::string name, kind; std::uintptr_t native = 0; };
    static std::mutex& m_() { static std::mutex m; return m; }
    static hooks& hooks_() { static hooks h; return h; }
    static std::uint64_t& next_() { static std::uint64_t n = 0; return n; }
    static std::map<std::uint64_t, task>& tasks_() { static std::map<std::uint64_t, task> t; return t; }
    static void release_(const task& t) {
#if defined(FORGE_OS_IOS)
        auto& o = detail::objc_api::get(); if (void* app = o.send(o.cls("UIApplication"), "sharedApplication")) o.send<void, std::uintptr_t>(app, "endBackgroundTask:", t.native);
#elif defined(__ANDROID__)
        if (hooks_().end) hooks_().end(t.name);
#elif defined(FORGE_OS_WINDOWS)
        auto& p = power_api_::get(); HANDLE h = reinterpret_cast<HANDLE>(t.native);
        if (h) { if (p.ok()) p.clear(h, 1); CloseHandle(h); }
#elif defined(FORGE_OS_MACOS)
        if (t.kind == "iopm-assertion") if (void* io = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY))
            if (auto rel = reinterpret_cast<int (*)(std::uint32_t)>(::dlsym(io, "IOPMAssertionRelease"))) rel(std::uint32_t(t.native));
#elif defined(FORGE_OS_LINUX)
        if (t.kind == "logind-inhibit") ::close(int(t.native));   // closing the fd releases the inhibitor
#else
        (void)t;
#endif
    }
#if defined(FORGE_OS_IOS)
    struct block_desc_t { unsigned long reserved, size; };
    struct block_t { void* isa; int flags; int reserved; void (*invoke)(block_t*); const block_desc_t* desc; std::uint64_t token; };
    static void expired_(block_t* b) { end(b->token); }   // iOS ran out of background time: end the task ourselves
#endif
#if defined(FORGE_OS_WINDOWS)
    struct power_api_ {
        struct reason_t { ULONG version; DWORD flags; union { struct { HMODULE module; ULONG id; ULONG count; LPWSTR* strings; } detailed; LPWSTR simple; }; };
        HANDLE (WINAPI* create)(reason_t*) = nullptr; BOOL (WINAPI* set)(HANDLE, int) = nullptr; BOOL (WINAPI* clear)(HANDLE, int) = nullptr;
        bool ok() const { return create && set && clear; }
        static power_api_& get() {
            static power_api_ a = [] { power_api_ p; HMODULE k = GetModuleHandleW(L"kernel32.dll"); if (!k) return p;
                p.create = reinterpret_cast<decltype(p.create)>(reinterpret_cast<void*>(GetProcAddress(k, "PowerCreateRequest")));
                p.set = reinterpret_cast<decltype(p.set)>(reinterpret_cast<void*>(GetProcAddress(k, "PowerSetRequest")));
                p.clear = reinterpret_cast<decltype(p.clear)>(reinterpret_cast<void*>(GetProcAddress(k, "PowerClearRequest"))); return p; }();
            return a;
        }
    };
#endif
#if defined(FORGE_OS_LINUX)
    // logind Inhibit("sleep:idle", ..., "block") through sd-bus. Returns the inhibitor fd, -1 when there is no
    // logind to inhibit (no libsystemd, no system bus, login1 not running), or an error when logind refused.
    static result<int> logind_inhibit_(const std::string& why) {
        struct bus_error { const char* name; const char* message; int need_free; };
        struct api {
            int (*open_system)(void**) = nullptr; int (*call)(void*, const char*, const char*, const char*, const char*, bus_error*, void**, const char*, ...) = nullptr;
            int (*read)(void*, const char*, ...) = nullptr; void* (*msg_unref)(void*) = nullptr; void* (*bus_unref)(void*) = nullptr; void (*err_free)(bus_error*) = nullptr;
            bool ok() const { return open_system && call && read && msg_unref && bus_unref && err_free; }
        };
        static api a = [] { api x; void* h = ::dlopen("libsystemd.so.0", RTLD_LAZY); if (!h) return x;
            x.open_system = reinterpret_cast<decltype(x.open_system)>(::dlsym(h, "sd_bus_open_system")); x.call = reinterpret_cast<decltype(x.call)>(::dlsym(h, "sd_bus_call_method"));
            x.read = reinterpret_cast<decltype(x.read)>(::dlsym(h, "sd_bus_message_read")); x.msg_unref = reinterpret_cast<decltype(x.msg_unref)>(::dlsym(h, "sd_bus_message_unref"));
            x.bus_unref = reinterpret_cast<decltype(x.bus_unref)>(::dlsym(h, "sd_bus_flush_close_unref")); x.err_free = reinterpret_cast<decltype(x.err_free)>(::dlsym(h, "sd_bus_error_free"));
            return x; }();
        if (!a.ok()) return {-1, {}};
        void* bus = nullptr; if (a.open_system(&bus) < 0 || !bus) return {-1, {}};
        std::string last; bool absent = false;
        for (const char* what : {"sleep:idle", "idle"}) {
            bus_error e{}; void* reply = nullptr;
            int r = a.call(bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit", &e, &reply, "ssss", what, "forge", why.c_str(), "block");
            if (r >= 0 && reply) {
                int fd = -1; int rr = a.read(reply, "h", &fd); int dup = rr >= 0 && fd >= 0 ? ::fcntl(fd, F_DUPFD_CLOEXEC, 3) : -1;
                a.msg_unref(reply); if (dup >= 0) { a.bus_unref(bus); return {dup, {}}; }
                last = "Inhibit reply without a file descriptor";
            } else {
                std::string n = e.name ? e.name : ""; last = e.message ? e.message : (n.empty() ? error_text(-r) : n);
                absent = n == "org.freedesktop.DBus.Error.ServiceUnknown" || n == "org.freedesktop.DBus.Error.NameHasNoOwner";
                if (reply) a.msg_unref(reply);
            }
            a.err_free(&e); if (absent) break;
        }
        a.bus_unref(bus);
        if (absent) return {-1, {}};
        return {-1, {EACCES, "background: logind Inhibit refused: " + last}};
    }
#endif
};
// RAII helper.
struct background_scope {
    std::uint64_t token = 0; explicit background_scope(const std::string& n) { auto r = background::begin(n); if (r) token = r.value; }
    ~background_scope() { if (token) background::end(token); }
};
} // namespace forge::os
