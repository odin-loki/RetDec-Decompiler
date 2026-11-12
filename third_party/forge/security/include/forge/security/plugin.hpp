#pragma once
// FSS INT-1 / INT-2 / INT-3 for native plugins: load a shared library only after the bytes that get mapped have passed
// the policy's checks.
//
//   auto lib = security::open_plugin(path);            // the active policy; os::result<os::library>
//   if (!lib) ... lib.err                               // the OS could not load it
//   // integrity failures throw security::integrity_error (audited INT-4 / INT-3), a denied capability permission_denied
//
// With no active policy, or integrity.plugins "off" and os_code_signing false, this is os::library::open(path). Otherwise:
//   1. enforce(plugin_load) (the capability; audited as before)
//   2. the file is opened once and read through that handle; integrity.plugins checks `<path>.sig` against those bytes
//      (verify_artefact); os_code_signing checks the OS code signature (os_code_signature)
//   3. the verified bytes are what gets loaded (TOCTOU):
//      Windows  the file stays open without FILE_SHARE_WRITE / FILE_SHARE_DELETE from the read until LoadLibraryExW
//               has mapped it, so nobody can modify, replace or rename it in between (a writer that already has it
//               open makes the open fail: the load is refused). WinVerifyTrust reads through the same handle.
//      Linux    the verified buffer is copied into an anonymous memfd and dlopen()ed from /proc/self/fd/N: the mapped
//      Android  bytes are the verified bytes whatever happens to the file. ($ORIGIN in the plugin's RUNPATH then
//               points to /proc/self/fd; plugins with private dependencies next to them need them on the normal
//               search path.) If memfd_create or that dlopen fails (old kernel, sandbox), step "others" applies.
//      others   dlopen(path) with an identity check: device, inode, size, mtime and ctime of the file read must equal
//               the path's before and after dlopen, else the library is closed again and integrity_error is thrown.
//               Residual race: a writer with write access to the file that rewrites it in place within one ctime
//               tick between the read and the mapping is not detected. Keep plugin directories non-writable by others.
#include <forge/security/enforce.hpp>
#include <forge/security/integrity.hpp>
#include <forge/os/os.hpp>
#include <string>
#include <vector>
#if defined(FORGE_OS_POSIX)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace forge::security {

// INT-3 decision for a plugin's OS code signature: signed passes (audited allow), "n/a" (no mechanism on this OS) is
// audited weaken and passes, anything else throws integrity_error (audited fail).
inline void apply_code_signing(const std::string& path, const code_signature& cs) {
    using S = code_signature::state;
    if (cs.st == S::signed_) { audit("integrity", decision::allow, "INT-3", path, "OS code signature valid" + (cs.signer.empty() ? std::string() : " (" + cs.signer + ")") + ", " + cs.backend); return; }
    if (cs.st == S::not_applicable) { audit("integrity", decision::weaken, "INT-3", path, "os_code_signing: " + cs.detail); return; }
    const std::string why = std::string("OS code signature ") + name(cs.st) + (cs.detail.empty() ? "" : " (" + cs.detail + ")") + ", " + cs.backend;
    audit("integrity", decision::fail, "INT-3", path, "plugin: " + why);
    throw integrity_error(path + ": " + why + " (integrity.os_code_signing)", "INT-3");
}

namespace detail {
#if defined(FORGE_OS_POSIX)
inline bool same_file(const struct stat& a, const struct stat& b) {
#if defined(__APPLE__)
    auto ts = [](const struct timespec& t) { return std::int64_t(t.tv_sec) * 1000000000 + t.tv_nsec; };
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && ts(a.st_mtimespec) == ts(b.st_mtimespec) && ts(a.st_ctimespec) == ts(b.st_ctimespec);
#else
    auto ts = [](const struct timespec& t) { return std::int64_t(t.tv_sec) * 1000000000 + t.tv_nsec; };
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && ts(a.st_mtim) == ts(b.st_mtim) && ts(a.st_ctim) == ts(b.st_ctim);
#endif
}
#endif
inline os::result<std::string> read_all(os::file& f, const std::string& path) {
    std::string s; char buf[65536];
    for (;;) {
        auto r = f.read(buf, sizeof buf);
        if (!r) return {{}, r.err};
        if (!r.value) break;
        s.append(buf, r.value);
        if (s.size() > (std::size_t(1) << 31)) return {{}, {-1, path + ": plugin larger than 2 GiB"}};
    }
    return {std::move(s), {}};
}
} // namespace detail

inline os::result<os::library> open_plugin(const std::string& path, const policy& p) {
    if (!p.active()) return os::library::open(path);
    {   // the capability; integrity is checked below on the bytes that are loaded, not by enforce's on-disk check
        policy q = p; q.integrity.plugins = requirement::off;
        if (auto s = try_enforce(q, capability::plugin_load, path); !s) throw permission_denied(s.err.what, "CAP-1");
    }
    const bool sig = p.integrity.plugins != requirement::off, ocs = p.integrity.os_code_signing;
    if (!sig && !ocs) return os::library::open(path);
#if defined(FORGE_OS_WINDOWS)
    const std::string abs = os::absolute(path);
    HANDLE h = CreateFileW(os::widen(abs).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        auto e = os::last_error("open " + path);
        if (GetLastError() == ERROR_SHARING_VIOLATION) e.what += " (the file is open for writing elsewhere; a verified plugin load needs it read-only)";
        return {{}, e};
    }
    os::file f(h);   // closed after LoadLibraryExW: no writer can open, replace or rename it until then
    auto b = detail::read_all(f, path);
    if (!b) return {{}, b.err};
    if (sig) verify_artefact(p, artefact::plugin, path, detail::bytes_of(b.value));
    if (ocs) { SetFilePointer(h, 0, nullptr, FILE_BEGIN); apply_code_signing(path, detail::win_code_signature(abs, h)); }
    HMODULE m = LoadLibraryExW(os::widen(abs).c_str(), nullptr, 0x8 /*LOAD_WITH_ALTERED_SEARCH_PATH*/);
    if (!m) return {{}, os::last_error("LoadLibrary " + path)};
    return {os::library::adopt(m), {}};
#elif defined(FORGE_OS_POSIX)
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {{}, os::last_error("open " + path)};
    os::file f(fd);
    struct stat st0 {}, st1 {};
    if (::fstat(fd, &st0) != 0) return {{}, os::last_error("fstat " + path)};
    if (!S_ISREG(st0.st_mode)) return {{}, {-1, path + ": not a regular file"}};
    auto b = detail::read_all(f, path);
    if (!b) return {{}, b.err};
    if (::fstat(fd, &st1) != 0 || !detail::same_file(st0, st1)) throw integrity_error(path + ": file changed while it was read", "INT-4");
    if (sig) verify_artefact(p, artefact::plugin, path, detail::bytes_of(b.value));
    if (ocs) apply_code_signing(path, os_code_signature(path));
#if defined(__linux__) && defined(SYS_memfd_create)
    if (int mfd = int(::syscall(SYS_memfd_create, "forge-plugin", 1u /*MFD_CLOEXEC*/)); mfd >= 0) {
        os::file mf(mfd);
        if (mf.write_all(b.value.data(), b.value.size())) {
            const std::string proc = "/proc/self/fd/" + std::to_string(mfd);
            if (void* hd = ::dlopen(proc.c_str(), RTLD_NOW | RTLD_LOCAL)) return {os::library::adopt(hd), {}};
        }
    }
#endif
    struct stat sa {}, sb {};
    if (::stat(path.c_str(), &sa) != 0 || !detail::same_file(st1, sa)) throw integrity_error(path + ": file replaced after it was verified", "INT-4");
    void* hd = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!hd) { const char* e = ::dlerror(); return {{}, {-1, e ? e : ("dlopen " + path)}}; }
    if (::stat(path.c_str(), &sb) != 0 || !detail::same_file(st1, sb)) { ::dlclose(hd); throw integrity_error(path + ": file replaced while it was loaded", "INT-4"); }
    return {os::library::adopt(hd), {}};
#else
    return os::library::open(path);
#endif
}
inline os::result<os::library> open_plugin(const std::string& path) { return open_plugin(path, *active_policy()); }

} // namespace forge::security
