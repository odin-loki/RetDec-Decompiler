#pragma once
// Named shared memory between processes.
//   POSIX (Linux, macOS, iOS-within-app-group, FreeBSD): shm_open + mmap.  Windows: CreateFileMappingW
//   ("Local\\forge-<name>").  Android has no shm_open: a file in shared_memory::directory() is mapped
//   MAP_SHARED, which other processes of the app / same uid can open by name. That directory defaults to
//   $TMPDIR; an Android app sets it with shared_memory::set_directory(<Context.getCacheDir()>) (the old default,
//   /data/local/tmp, is not writable by apps).
// create() makes a new region and fails with already_exists if the name is taken (unlink() a stale one first);
// open() attaches to an existing one. Apple limits names to 31 bytes: longer names are shortened with a hash
// of the full name, so distinct long names stay distinct. Windows: open(name, 0) reports the size rounded up
// to whole pages (a section does not record the size it was created with).
#include <forge/os/detail.hpp>

namespace forge::os {
class shared_memory {
public:
    shared_memory() = default;
    shared_memory(shared_memory&& o) noexcept { *this = std::move(o); }
    shared_memory& operator=(shared_memory&& o) noexcept {
        if (this != &o) { close(); p_ = std::exchange(o.p_, nullptr); n_ = std::exchange(o.n_, 0); name_ = std::move(o.name_); owner_ = std::exchange(o.owner_, false);
#if defined(FORGE_OS_WINDOWS)
            h_ = std::exchange(o.h_, nullptr);
#endif
        }
        return *this;
    }
    ~shared_memory() { close(); }
    // Create a new region of n bytes (zero-filled). The creator unlinks the name on close unless keep() is called.
    static result<shared_memory> create(const std::string& name, std::size_t n) { return open_(name, n, true); }
    // Open an existing region; n = 0 means "whatever size it has".
    static result<shared_memory> open(const std::string& name, std::size_t n = 0) { return open_(name, n, false); }
    std::uint8_t* data() const { return static_cast<std::uint8_t*>(p_); }
    std::size_t size() const { return n_; }
    const std::string& name() const { return name_; }
    void keep() { owner_ = false; }
    static status unlink(const std::string& name) {
#if defined(FORGE_OS_WINDOWS)
        (void)name; return {};   // named mappings vanish with their last handle
#elif defined(__ANDROID__)
        auto p = path_(name); if (p.empty()) return {{}, no_dir_()};
        if (::unlink(p.c_str())) return {{}, last_error("unlink")}; return {};
#else
        if (::shm_unlink(path_(name).c_str())) return {{}, last_error("shm_unlink")}; return {};
#endif
    }
    // Android: where region files live (default $TMPDIR). Ignored elsewhere.
    static void set_directory(const std::string& dir) { std::lock_guard<std::mutex> g(dir_mu_()); dir_() = dir; }
    static std::string directory() {
        std::lock_guard<std::mutex> g(dir_mu_());
        if (!dir_().empty()) return dir_();
        return getenv("TMPDIR");
    }
    // The OS-level object name for `name` (shm_open path, Windows object name, or Android file path).
    static std::string os_name(const std::string& name) { return path_(name); }
    void close() {
        if (!p_) return;
#if defined(FORGE_OS_WINDOWS)
        UnmapViewOfFile(p_); if (h_) CloseHandle(h_); h_ = nullptr;
#else
        ::munmap(p_, n_); if (owner_) unlink(name_);
#endif
        p_ = nullptr; n_ = 0;
    }
private:
    static std::string& dir_() { static std::string d; return d; }
    static std::mutex& dir_mu_() { static std::mutex m; return m; }
    static error no_dir_() { return errno_error(ENOENT, "shared_memory: no directory (Android: call shared_memory::set_directory with the app cache dir, or set TMPDIR)"); }
    static std::string path_(const std::string& name) {
#if defined(FORGE_OS_WINDOWS)
        return "Local\\forge-" + name;
#elif defined(__ANDROID__)
        auto d = directory(); return d.empty() ? std::string() : join(d, "forge-shm-" + name);
#elif defined(__APPLE__)
        // PSHMNAMLEN is 31. Short names map 1:1; longer ones keep a readable prefix plus a 64-bit FNV-1a hash of
        // the whole name ("/fg-" + 16 hex + "-" + 9 chars = 30).
        if (name.size() <= 26) return "/fg-" + name;
        std::uint64_t h = 1469598103934665603ull; for (unsigned char c : name) { h ^= c; h *= 1099511628211ull; }
        char hex[17]; std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
        return "/fg-" + std::string(hex) + "-" + name.substr(0, 9);
#else
        return "/forge-" + name;
#endif
    }
    static result<shared_memory> open_(const std::string& name, std::size_t n, bool create) {
        shared_memory s; s.name_ = name;
        if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return {std::move(s), errno_error(EINVAL, "shared_memory: bad name")};
        if (create && !n) return {std::move(s), errno_error(EINVAL, "shared_memory: create with size 0")};
#if defined(FORGE_OS_WINDOWS)
        auto w = widen(path_(name));
        if (create) {
            s.h_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(std::uint64_t(n) >> 32), DWORD(n), w.c_str());
            if (s.h_ && GetLastError() == ERROR_ALREADY_EXISTS) {   // CreateFileMapping opens an existing mapping silently
                CloseHandle(s.h_); s.h_ = nullptr; SetLastError(ERROR_ALREADY_EXISTS);
            }
        } else s.h_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, w.c_str());
        if (!s.h_) return {std::move(s), last_error("shared_memory " + name)};
        s.p_ = MapViewOfFile(s.h_, FILE_MAP_ALL_ACCESS, 0, 0, n);
        if (!s.p_) { auto e = last_error("MapViewOfFile"); CloseHandle(s.h_); s.h_ = nullptr; return {std::move(s), e}; }
        MEMORY_BASIC_INFORMATION mi{}; VirtualQuery(s.p_, &mi, sizeof mi); s.n_ = n ? n : mi.RegionSize;
        s.owner_ = create;
        return {std::move(s), {}};
#else
        auto p = path_(name);
        // create: O_EXCL, so an existing region (another process's, or a stale one) is never truncated under its users.
        int fl = O_RDWR | (create ? O_CREAT | O_EXCL : 0);
#if defined(__ANDROID__)
        if (p.empty()) return {std::move(s), no_dir_()};
        int fd = ::open(p.c_str(), fl | O_CLOEXEC | O_NOFOLLOW, 0600);
#else
        int fd = ::shm_open(p.c_str(), fl, 0600);
#endif
        if (fd < 0) return {std::move(s), last_error("shm_open " + name)};
        auto fail = [&](error e) { ::close(fd); if (create) unlink(name); return result<shared_memory>{std::move(s), std::move(e)}; };
        if (create && ::ftruncate(fd, off_t(n))) return fail(last_error("ftruncate"));
        if (!n) { struct ::stat st; if (::fstat(fd, &st) == 0) n = std::size_t(st.st_size); }
        if (!n) return fail(errno_error(EINVAL, "shared_memory: empty region"));
        void* m = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) return fail(last_error("mmap shared"));
        ::close(fd);
        s.p_ = m; s.n_ = n; s.owner_ = create; return {std::move(s), {}};
#endif
    }
    void* p_ = nullptr; std::size_t n_ = 0; std::string name_; bool owner_ = false;
#if defined(FORGE_OS_WINDOWS)
    HANDLE h_ = nullptr;
#endif
};
} // namespace forge::os
