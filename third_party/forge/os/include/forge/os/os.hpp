#pragma once
// forge::os: one OS-call API for Windows, Linux, Android, macOS, iOS and FreeBSD.
// Files and fs, mmap, processes, threads, sync, time, sockets and polling (epoll / kqueue / WSAPoll), dynamic
// libraries, environment, signals, memory, system and CPU info, paths, and entropy.
// Extension modules (numa, hugepages, async_file, cpu_topology, power, memory_pressure, shared_memory, gpu_devices,
// local_ipc, crash, sandbox, keystore, file_watch, perf, background, service) live beside this header; include
// <forge/os/all.hpp> for all of them. Missing OS features return an error where os::is_unsupported(err) is true.
// Guide: docs/os.md.
// Errors: calls return os::result<T> (value or a native error code with a message); nothing throws.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define FORGE_OS_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602   // PrefetchVirtualMemory etc. (llvm-mingw defaults lower)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#define FORGE_OS_POSIX 1
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/event.h>
#include <sys/sysctl.h>
#define FORGE_OS_KQUEUE 1
#if TARGET_OS_IPHONE
#define FORGE_OS_IOS 1
#else
#define FORGE_OS_MACOS 1
#endif
#elif defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/sysctl.h>
#define FORGE_OS_KQUEUE 1
#define FORGE_OS_FREEBSD 1
#include <pthread_np.h>
#include <sys/param.h>
#include <sys/cpuset.h>
#include <sys/rtprio.h>
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/syscall.h>
#define FORGE_OS_EPOLL 1
#if defined(__ANDROID__)
#define FORGE_OS_ANDROID 1
#else
#define FORGE_OS_LINUX 1
#include <sys/random.h>
#endif
#endif
#if !defined(FORGE_OS_IOS)
#include <spawn.h>
extern char** environ;
#endif
#endif

namespace forge::os {

// ---------------------------------------------------------------- fork
// One process-wide lock around every fork() FORGE does (spawn_wait,
// child::start, sandbox, exec fallback), and for an embedding program's own
// forks: a library that registers a pthread_atfork prepare handler taking its
// own locks (Z3's scoped_timer pool, for one) livelocks when two threads fork
// at once. Header-only, one mutex per process (inline function, ODR).
// POSIX only; Windows has no fork.
#if !defined(FORGE_OS_WINDOWS)
inline std::mutex& fork_mutex() noexcept {
    static std::mutex m;
    return m;
}
inline pid_t serialized_fork() noexcept {
    std::lock_guard<std::mutex> lk(fork_mutex());
    return ::fork();
}
// Descriptors born close-on-exec, so a fork+exec on another thread never inherits them (an inherited pipe write
// end keeps the reader from seeing EOF). Linux, Android and FreeBSD create them atomically (SOCK_CLOEXEC,
// pipe2, accept4); Apple has none of those, so creation and FD_CLOEXEC happen under fork_mutex(), which covers
// every fork FORGE does (serialized_fork). accept on Apple stays a small window: holding the fork lock across a
// blocking accept would stall every fork.
inline int cloexec_socket(int family, int type, int proto = 0) {
#if defined(SOCK_CLOEXEC)
    return ::socket(family, type | SOCK_CLOEXEC, proto);
#else
    std::lock_guard<std::mutex> lk(fork_mutex());
    int s = ::socket(family, type, proto); if (s >= 0) ::fcntl(s, F_SETFD, FD_CLOEXEC); return s;
#endif
}
inline int cloexec_pipe(int fds[2]) {
#if defined(__APPLE__)
    std::lock_guard<std::mutex> lk(fork_mutex());
    if (::pipe(fds)) return -1;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC); ::fcntl(fds[1], F_SETFD, FD_CLOEXEC); return 0;
#else
    return ::pipe2(fds, O_CLOEXEC);
#endif
}
inline int cloexec_accept(int fd) {
    for (;;) {
#if defined(SOCK_CLOEXEC) && !defined(__APPLE__)
        int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
        int c = ::accept(fd, nullptr, nullptr); if (c >= 0) ::fcntl(c, F_SETFD, FD_CLOEXEC);
#endif
        if (c >= 0 || errno != EINTR) return c;
    }
}
#endif

// ---------------------------------------------------------------- errors
// Which number space `code` is in: native = errno on POSIX, GetLastError()/WSAGetLastError() on Windows; posix =
// an errno value on any OS (the C runtime, libstdc++'s std::filesystem on mingw); win32 = a Win32 code; gai = a
// getaddrinfo EAI_* value (negative on glibc, small positives that collide with errno on BSD/Apple).
enum class err_domain : std::uint8_t { native, posix, win32, gai };
struct error {
    int code = 0; std::string what; err_domain domain = err_domain::native;
    explicit operator bool() const { return code != 0; }
};
inline error errno_error(int c, std::string what) { return {c, std::move(what), err_domain::posix}; }
inline int last_errno() {
#if defined(FORGE_OS_WINDOWS)
    return int(GetLastError());
#else
    return errno;
#endif
}
inline std::string error_text(int code) {
#if defined(FORGE_OS_WINDOWS)
    char b[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(code), 0, b, sizeof b, nullptr);
    std::string s(b); while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back(); return s;
#else
    return std::strerror(code);
#endif
}
inline error last_error(std::string_view ctx) { int c = last_errno(); if (!c) c = -1; return {c, std::string(ctx) + ": " + error_text(c)}; }
template <class T> struct result {
    T value{}; error err;
    explicit operator bool() const { return !err; }
    T& operator*() { return value; }
    T* operator->() { return &value; }
};
struct ok_t {};
using status = result<ok_t>;
// A feature this OS (or this build) does not provide: callers get a clean error instead of a crash.
inline constexpr int unsupported_code = 0x0F0E5;
inline error unsupported(std::string_view what) { return {unsupported_code, std::string(what) + ": unsupported on this platform"}; }
inline bool is_unsupported(const error& e) { return e.code == unsupported_code; }

// ---------------------------------------------------------------- platform identity
enum class platform { windows, linux, android, macos, ios, freebsd, other };
constexpr platform current() {
#if defined(FORGE_OS_WINDOWS)
    return platform::windows;
#elif defined(FORGE_OS_ANDROID)
    return platform::android;
#elif defined(FORGE_OS_LINUX)
    return platform::linux;
#elif defined(FORGE_OS_IOS)
    return platform::ios;
#elif defined(FORGE_OS_MACOS)
    return platform::macos;
#elif defined(FORGE_OS_FREEBSD)
    return platform::freebsd;
#else
    return platform::other;
#endif
}
constexpr const char* platform_name(platform p = current()) {
    switch (p) { case platform::windows: return "windows"; case platform::linux: return "linux"; case platform::android: return "android";
                 case platform::macos: return "macos"; case platform::ios: return "ios"; case platform::freebsd: return "freebsd"; default: return "other"; }
}

#if defined(FORGE_OS_WINDOWS)
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
inline std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr); return s;
}
// CreateProcessW whose child inherits only si's three std handles (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). Plain
// bInheritHandles=TRUE also hands the child every inheritable handle other threads hold at that moment, such
// as another child's pipe ends; that pipe then reports no EOF until the unrelated child exits.
inline BOOL create_process_std_only(const wchar_t* app, wchar_t* cmd, DWORD flags, void* env, const wchar_t* cwd,
                                    const STARTUPINFOW& si, PROCESS_INFORMATION& pi) {
    HANDLE self = GetCurrentProcess();
    STARTUPINFOEXW sx{}; sx.StartupInfo = si; sx.StartupInfo.cb = sizeof sx;
    HANDLE* std3[3] = {&sx.StartupInfo.hStdInput, &sx.StartupInfo.hStdOutput, &sx.StartupInfo.hStdError};
    HANDLE orig[3] = {}, dups[3] = {}; DWORD n = 0;
    if (si.dwFlags & STARTF_USESTDHANDLES)
        for (HANDLE* p : std3) {   // inheritable duplicates, so std handles the parent opened non-inheritable work too
            if (!*p || *p == INVALID_HANDLE_VALUE) continue;
            DWORD k = 0; while (k < n && orig[k] != *p) ++k;
            if (k == n) { if (!DuplicateHandle(self, *p, self, &dups[n], 0, TRUE, DUPLICATE_SAME_ACCESS)) { *p = nullptr; continue; } orig[n++] = *p; }
            *p = dups[k];
        }
    SIZE_T sz = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<char> buf(sz ? sz : 1);
    auto* al = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf.data());
    bool init = n && InitializeProcThreadAttributeList(al, 1, 0, &sz);
    bool attr = init && UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, dups, n * sizeof(HANDLE), nullptr, nullptr);
    if (attr) sx.lpAttributeList = al;
    BOOL ok = CreateProcessW(app, cmd, nullptr, nullptr, n ? TRUE : FALSE, flags | (attr ? EXTENDED_STARTUPINFO_PRESENT : 0), env, cwd,
                             &sx.StartupInfo, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (init) DeleteProcThreadAttributeList(al);
    for (DWORD i = 0; i < n; ++i) CloseHandle(dups[i]);
    if (!ok) SetLastError(err);
    return ok;
}
#endif

// ---------------------------------------------------------------- files
enum open_flags : unsigned { rd = 1, wr = 2, rdwr = 3, create = 4, truncate = 8, append = 16, excl = 32 };
class file {
public:
#if defined(FORGE_OS_WINDOWS)
    using native_t = HANDLE;
    static native_t bad() { return INVALID_HANDLE_VALUE; }
#else
    using native_t = int;
    static native_t bad() { return -1; }
#endif
    file() = default;
    explicit file(native_t h) : h_(h) {}
    file(file&& o) noexcept : h_(std::exchange(o.h_, bad())) {}
    file& operator=(file&& o) noexcept { if (this != &o) { close(); h_ = std::exchange(o.h_, bad()); } return *this; }
    ~file() { close(); }
    bool valid() const { return h_ != bad(); }
    native_t native() const { return h_; }
    native_t release() { return std::exchange(h_, bad()); }   // give up ownership
    void close() {
        if (!valid()) return;
#if defined(FORGE_OS_WINDOWS)
        CloseHandle(h_);
#else
        ::close(h_);
#endif
        h_ = bad();
    }
    static result<file> open(const std::string& path, unsigned flags = rd) {
#if defined(FORGE_OS_WINDOWS)
        DWORD acc = ((flags & rd) ? GENERIC_READ : 0) | ((flags & (wr | append)) ? GENERIC_WRITE : 0);
        DWORD disp = (flags & excl) ? CREATE_NEW : (flags & create) ? ((flags & truncate) ? CREATE_ALWAYS : OPEN_ALWAYS)
                   : (flags & truncate) ? TRUNCATE_EXISTING : OPEN_EXISTING;
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE h = CreateFileW(widen(path).c_str(), acc, share, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {{}, last_error("open " + path)};
        if (flags & append) {   // O_APPEND: a handle with FILE_APPEND_DATA but no FILE_WRITE_DATA writes at the end atomically
            DWORD a = (FILE_GENERIC_WRITE & ~DWORD(FILE_WRITE_DATA)) | ((flags & rd) ? FILE_GENERIC_READ : 0);
            HANDLE r = ReOpenFile(h, a, share, 0);
            if (r == INVALID_HANDLE_VALUE) { auto e = last_error("open " + path); CloseHandle(h); return {{}, e}; }
            CloseHandle(h); h = r;
        }
        return {file(h), {}};
#else
        int f = ((flags & rdwr) == rdwr || ((flags & rd) && (flags & append))) ? O_RDWR : (flags & (wr | append)) ? O_WRONLY : O_RDONLY;
        if (flags & create) f |= O_CREAT; if (flags & truncate) f |= O_TRUNC; if (flags & append) f |= O_APPEND; if (flags & excl) f |= O_EXCL | O_CREAT;
        int h = ::open(path.c_str(), f | O_CLOEXEC, 0644);
        if (h < 0) return {{}, last_error("open " + path)};
        return {file(h), {}};
#endif
    }
    // Windows moves at most 1 GiB per call (DWORD lengths): reads and writes may be short, as POSIX allows.
    // pread/pwrite on Windows: a synchronous handle's file position ends up after the transferred range (the OS
    // updates it for OVERLAPPED I/O too), so do not mix them with read()/write() on the same handle.
#if defined(FORGE_OS_WINDOWS)
    static DWORD chunk(std::size_t n) { return DWORD(n < (std::size_t(1) << 30) ? n : (std::size_t(1) << 30)); }
#endif
    result<std::size_t> read(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
        DWORD got = 0; if (!ReadFile(h_, p, chunk(n), &got, nullptr)) { if (GetLastError() == ERROR_BROKEN_PIPE) return {0, {}}; return {0, last_error("read")}; } return {got, {}};
#else
        for (;;) { auto r = ::read(h_, p, n); if (r >= 0) return {std::size_t(r), {}}; if (errno != EINTR) return {0, last_error("read")}; }
#endif
    }
    result<std::size_t> write(const void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
        DWORD put = 0; if (!WriteFile(h_, p, chunk(n), &put, nullptr)) return {0, last_error("write")}; return {put, {}};
#else
        for (;;) { auto r = ::write(h_, p, n); if (r >= 0) return {std::size_t(r), {}}; if (errno != EINTR) return {0, last_error("write")}; }
#endif
    }
    result<std::size_t> pread(void* p, std::size_t n, std::uint64_t off) {
#if defined(FORGE_OS_WINDOWS)
        OVERLAPPED o{}; o.Offset = DWORD(off); o.OffsetHigh = DWORD(off >> 32); DWORD got = 0;
        if (!ReadFile(h_, p, chunk(n), &got, &o)) { if (GetLastError() == ERROR_HANDLE_EOF) return {0, {}}; return {0, last_error("pread")}; }
        return {got, {}};
#else
        for (;;) { auto r = ::pread(h_, p, n, off_t(off)); if (r >= 0) return {std::size_t(r), {}}; if (errno != EINTR) return {0, last_error("pread")}; }
#endif
    }
    result<std::size_t> pwrite(const void* p, std::size_t n, std::uint64_t off) {
#if defined(FORGE_OS_WINDOWS)
        OVERLAPPED o{}; o.Offset = DWORD(off); o.OffsetHigh = DWORD(off >> 32); DWORD put = 0;
        if (!WriteFile(h_, p, chunk(n), &put, &o)) return {0, last_error("pwrite")}; return {put, {}};
#else
        for (;;) { auto r = ::pwrite(h_, p, n, off_t(off)); if (r >= 0) return {std::size_t(r), {}}; if (errno != EINTR) return {0, last_error("pwrite")}; }
#endif
    }
    status write_all(const void* p, std::size_t n) {
        auto* b = static_cast<const std::uint8_t*>(p);
        while (n) { auto r = write(b, n); if (!r) return {{}, r.err}; if (!r.value) return {{}, {-1, "write: no progress"}}; b += r.value; n -= r.value; }
        return {};
    }
    result<std::uint64_t> size() const {
#if defined(FORGE_OS_WINDOWS)
        LARGE_INTEGER s{}; if (!GetFileSizeEx(h_, &s)) return {0, last_error("size")}; return {std::uint64_t(s.QuadPart), {}};
#else
        struct stat st{}; if (::fstat(h_, &st)) return {0, last_error("fstat")}; return {std::uint64_t(st.st_size), {}};
#endif
    }
    status resize(std::uint64_t n) {
#if defined(FORGE_OS_WINDOWS)
        FILE_END_OF_FILE_INFO i{}; i.EndOfFile.QuadPart = LONGLONG(n);
        if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &i, sizeof i)) return {{}, last_error("resize")}; return {};
#else
        if (::ftruncate(h_, off_t(n))) return {{}, last_error("ftruncate")}; return {};
#endif
    }
    status sync() {
#if defined(FORGE_OS_WINDOWS)
        if (!FlushFileBuffers(h_)) return {{}, last_error("fsync")}; return {};
#elif defined(__APPLE__)
        if (::fcntl(h_, F_FULLFSYNC) && ::fsync(h_)) return {{}, last_error("fsync")}; return {};
#else
        if (::fsync(h_)) return {{}, last_error("fsync")}; return {};
#endif
    }
private:
    native_t h_ = bad();
};

inline result<std::string> read_file(const std::string& path) {
    auto f = file::open(path, rd); if (!f) return {{}, f.err};
    std::string s; char buf[65536];
    for (;;) { auto r = f->read(buf, sizeof buf); if (!r) return {{}, r.err}; if (!r.value) break; s.append(buf, r.value); }
    return {std::move(s), {}};
}
inline status write_file(const std::string& path, std::string_view data) {
    auto f = file::open(path, wr | create | truncate); if (!f) return {{}, f.err};
    return f->write_all(data.data(), data.size());
}

// ---------------------------------------------------------------- filesystem
struct file_info { bool exists = false, is_dir = false, is_file = false, is_link = false; std::uint64_t size = 0; std::int64_t mtime_ns = 0; };
inline file_info stat(const std::string& path) {
    file_info i;
#if defined(FORGE_OS_WINDOWS)
    // Like POSIX stat(): links are followed for type, size and mtime, and is_link says the path itself is one.
    // Only symbolic links and junctions are links: other reparse points (OneDrive / cloud placeholders, dedup,
    // WSL, app-exec aliases) are ordinary files and directories. A link whose target cannot be opened (dangling,
    // or a WSL symlink Windows cannot follow) reports the link's own attributes, so it can still be removed.
    auto w = widen(path);
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &d)) return i;
    auto ft = [](LARGE_INTEGER v) { return (std::int64_t(v.QuadPart) - 116444736000000000LL) * 100; };
    i.exists = true; i.is_dir = d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY; i.is_file = !i.is_dir && !(d.dwFileAttributes & FILE_ATTRIBUTE_DEVICE);
    i.size = i.is_dir ? 0 : (std::uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    LARGE_INTEGER t; t.LowPart = d.ftLastWriteTime.dwLowDateTime; t.HighPart = LONG(d.ftLastWriteTime.dwHighDateTime); i.mtime_ns = ft(t);
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        HANDLE h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (h != INVALID_HANDLE_VALUE) { if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof tag)) i.is_link = tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT; CloseHandle(h); }
        if (i.is_link) {
            h = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES, share, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // follows the link
            FILE_BASIC_INFO b{}; FILE_STANDARD_INFO s{};
            if (h != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(h, FileBasicInfo, &b, sizeof b) && GetFileInformationByHandleEx(h, FileStandardInfo, &s, sizeof s)) {
                i.is_dir = s.Directory; i.is_file = !i.is_dir && !(b.FileAttributes & FILE_ATTRIBUTE_DEVICE);
                i.size = i.is_dir ? 0 : std::uint64_t(s.EndOfFile.QuadPart); i.mtime_ns = ft(b.LastWriteTime);
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
#else
    struct stat st{}, lst{};
    if (::stat(path.c_str(), &st)) return i;
    i.exists = true; i.is_dir = S_ISDIR(st.st_mode); i.is_file = S_ISREG(st.st_mode); i.size = std::uint64_t(st.st_size);
    if (!::lstat(path.c_str(), &lst)) i.is_link = S_ISLNK(lst.st_mode);
#if defined(__APPLE__)
    i.mtime_ns = std::int64_t(st.st_mtimespec.tv_sec) * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    i.mtime_ns = std::int64_t(st.st_mtim.tv_sec) * 1000000000 + st.st_mtim.tv_nsec;
#endif
#endif
    return i;
}
inline bool exists(const std::string& p) { return stat(p).exists; }
inline status mkdir(const std::string& p) {
#if defined(FORGE_OS_WINDOWS)
    if (!CreateDirectoryW(widen(p).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {{}, last_error("mkdir " + p)};
#else
    if (::mkdir(p.c_str(), 0755) && errno != EEXIST) return {{}, last_error("mkdir " + p)};
#endif
    return {};
}
inline bool is_sep(char c) {
#if defined(FORGE_OS_WINDOWS)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}
inline status mkdirs(const std::string& p) {
    for (std::size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || is_sep(p[i])) {
            std::string sub = p.substr(0, i);
            if (sub.empty() || (sub.size() == 2 && sub[1] == ':')) continue;
            if (!stat(sub).is_dir) if (auto s = mkdir(sub); !s) return s;
        }
    return {};
}
inline status remove(const std::string& p) {
#if defined(FORGE_OS_WINDOWS)
    auto w = widen(p);
    if (stat(p).is_dir ? !RemoveDirectoryW(w.c_str()) : !DeleteFileW(w.c_str())) return {{}, last_error("remove " + p)};
#else
    if (::remove(p.c_str())) return {{}, last_error("remove " + p)};
#endif
    return {};
}
inline status rename(const std::string& from, const std::string& to) {   // atomic replace where the OS allows
#if defined(FORGE_OS_WINDOWS)
    if (!MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return {{}, last_error("rename " + from)};
#else
    if (::rename(from.c_str(), to.c_str())) return {{}, last_error("rename " + from)};
#endif
    return {};
}
struct dir_entry { std::string name; bool is_dir = false; };
inline result<std::vector<dir_entry>> list_dir(const std::string& p) {
    std::vector<dir_entry> out;
#if defined(FORGE_OS_WINDOWS)
    WIN32_FIND_DATAW d{}; HANDLE h = FindFirstFileW(widen(p + "\\*").c_str(), &d);
    if (h == INVALID_HANDLE_VALUE) return {{}, last_error("list " + p)};
    do { std::string n = narrow(d.cFileName); if (n != "." && n != "..") out.push_back({n, bool(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)}); } while (FindNextFileW(h, &d));
    FindClose(h);
#else
    DIR* d = ::opendir(p.c_str()); if (!d) return {{}, last_error("list " + p)};
    while (auto* e = ::readdir(d)) {
        std::string n = e->d_name; if (n == "." || n == "..") continue;
        bool dir = e->d_type == DT_DIR; if (e->d_type == DT_UNKNOWN) dir = stat(p + "/" + n).is_dir;
        out.push_back({n, dir});
    }
    ::closedir(d);
#endif
    return {std::move(out), {}};
}
inline status remove_all(const std::string& p) {
    auto i = stat(p); if (!i.exists) return {};
    if (i.is_dir && !i.is_link) { auto l = list_dir(p); if (!l) return {{}, l.err}; for (auto& e : l.value) if (auto s = remove_all(p + "/" + e.name); !s) return s; }
    return remove(p);
}

// ---------------------------------------------------------------- paths
inline std::string join(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b); if (b.empty()) return std::string(a);
    std::string s(a); if (!is_sep(s.back())) s += '/'; s += b; return s;
}
inline std::string basename(std::string_view p) { std::size_t i = p.size(); while (i && !is_sep(p[i - 1])) --i; return std::string(p.substr(i)); }
inline std::string dirname(std::string_view p) { std::size_t i = p.size(); while (i && !is_sep(p[i - 1])) --i; if (!i) return "."; return std::string(p.substr(0, i > 1 ? i - 1 : 1)); }
inline std::string temp_dir() {
#if defined(FORGE_OS_WINDOWS)
    std::wstring b(MAX_PATH + 1, L'\0'); DWORD n = GetTempPathW(DWORD(b.size()), b.data());
    if (n >= b.size()) { b.assign(std::size_t(n) + 1, L'\0'); n = GetTempPathW(DWORD(b.size()), b.data()); if (n >= b.size()) n = 0; }   // TMP longer than MAX_PATH
    std::string s = narrow({b.data(), n}); while (s.size() > 3 && is_sep(s.back())) s.pop_back(); return s;
#else
    for (const char* v : {"TMPDIR", "TMP", "TEMP"}) if (const char* t = std::getenv(v); t && *t) { std::string s = t; while (s.size() > 1 && s.back() == '/') s.pop_back(); return s; }
#if defined(FORGE_OS_ANDROID)
    return "/data/local/tmp";
#else
    return "/tmp";
#endif
#endif
}
inline std::string cwd() {
#if defined(FORGE_OS_WINDOWS)
    wchar_t b[32768]; DWORD n = GetCurrentDirectoryW(32768, b); return narrow({b, n});
#else
    char b[4096]; return ::getcwd(b, sizeof b) ? b : "";
#endif
}
inline status chdir(const std::string& p) {
#if defined(FORGE_OS_WINDOWS)
    if (!SetCurrentDirectoryW(widen(p).c_str())) return {{}, last_error("chdir")};
#else
    if (::chdir(p.c_str())) return {{}, last_error("chdir")};
#endif
    return {};
}
inline std::string exe_path() {
#if defined(FORGE_OS_WINDOWS)
    wchar_t b[32768]; DWORD n = GetModuleFileNameW(nullptr, b, 32768); return narrow({b, n});
#elif defined(__APPLE__)
    char b[4096]; std::uint32_t n = sizeof b; if (_NSGetExecutablePath(b, &n)) return ""; char r[4096]; return ::realpath(b, r) ? r : b;
#elif defined(FORGE_OS_FREEBSD)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1}; char b[4096]; std::size_t n = sizeof b;
    return ::sysctl(mib, 4, b, &n, nullptr, 0) ? "" : b;
#else
    char b[4096]; auto n = ::readlink("/proc/self/exe", b, sizeof b - 1); return n > 0 ? std::string(b, std::size_t(n)) : "";
#endif
}
inline std::string absolute(const std::string& p) {
#if defined(FORGE_OS_WINDOWS)
    wchar_t b[32768]; DWORD n = GetFullPathNameW(widen(p).c_str(), 32768, b, nullptr); return n ? narrow({b, n}) : p;
#else
    char b[PATH_MAX]; if (::realpath(p.c_str(), b)) return b; return p.size() && p[0] == '/' ? p : join(cwd(), p);
#endif
}
inline std::string getenv(const std::string&, const std::string& = {});
inline std::string home_dir() {
#if defined(FORGE_OS_WINDOWS)
    return getenv("USERPROFILE");
#else
    return getenv("HOME");
#endif
}

// ---------------------------------------------------------------- memory mapping
enum prot : unsigned { prot_read = 1, prot_write = 2, prot_exec = 4 };
// File-mapping offsets must be multiples of this: the page size on POSIX, 64 KiB on Windows.
inline std::size_t allocation_granularity() {
#if defined(FORGE_OS_WINDOWS)
    SYSTEM_INFO s; GetSystemInfo(&s); return s.dwAllocationGranularity;
#else
    return std::size_t(::sysconf(_SC_PAGESIZE));
#endif
}
class mapping {
public:
    mapping() = default;
    mapping(mapping&& o) noexcept : p_(std::exchange(o.p_, nullptr)), n_(std::exchange(o.n_, 0)) {}
    mapping& operator=(mapping&& o) noexcept { if (this != &o) { reset(); p_ = std::exchange(o.p_, nullptr); n_ = std::exchange(o.n_, 0); } return *this; }
    ~mapping() { reset(); }
    std::uint8_t* data() const { return static_cast<std::uint8_t*>(p_); }
    std::size_t size() const { return n_; }
    void* release() { n_ = 0; return std::exchange(p_, nullptr); }
    void reset() {
        if (!p_) return;
#if defined(FORGE_OS_WINDOWS)
        UnmapViewOfFile(p_);
#else
        ::munmap(p_, n_);
#endif
        p_ = nullptr; n_ = 0;
    }
    // Map a file (from off to the end if len == 0). Writable maps are shared (writes reach the file). off must be a
    // multiple of allocation_granularity() and lie within the file; off + len past the end is refused (touching
    // such pages faults on POSIX, and Windows cannot map them without growing the file).
    static result<mapping> map_file(const std::string& path, unsigned pr = prot_read, std::uint64_t off = 0, std::size_t len = 0) {
        if (off % allocation_granularity()) return {{}, errno_error(EINVAL, "map " + path + ": offset not a multiple of the allocation granularity")};
        auto f = file::open(path, (pr & prot_write) ? rdwr : rd); if (!f) return {{}, f.err};
        auto sz = f->size(); if (!sz) return {{}, sz.err};
        if (off > sz.value || std::uint64_t(len) > sz.value - off) return {{}, errno_error(EINVAL, "map " + path + ": range past the end of the file")};
        if (!len) { if (sz.value - off > std::uint64_t(SIZE_MAX)) return {{}, errno_error(EFBIG, "map " + path + ": too large")}; len = std::size_t(sz.value - off); }
        mapping m; if (!len) return {std::move(m), {}};
#if defined(FORGE_OS_WINDOWS)
        bool wr = pr & prot_write, ex = pr & prot_exec;
        HANDLE h = CreateFileMappingW(f->native(), nullptr, ex ? (wr ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : wr ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
        if (!h) return {{}, last_error("CreateFileMapping " + path)};
        void* p = MapViewOfFile(h, (wr ? FILE_MAP_WRITE : FILE_MAP_READ) | (ex ? FILE_MAP_EXECUTE : 0), DWORD(off >> 32), DWORD(off), len);
        CloseHandle(h);
        if (!p) return {{}, last_error("MapViewOfFile " + path)};
        if (!(pr & (prot_read | prot_write | prot_exec))) { DWORD old; VirtualProtect(p, len, PAGE_NOACCESS, &old); }
#else
        int pp = ((pr & prot_read) ? PROT_READ : 0) | ((pr & prot_write) ? PROT_WRITE : 0) | ((pr & prot_exec) ? PROT_EXEC : 0);
        void* p = ::mmap(nullptr, len, pp, (pr & prot_write) ? MAP_SHARED : MAP_PRIVATE, f->native(), off_t(off));
        if (p == MAP_FAILED) return {{}, last_error("mmap " + path)};
#endif
        m.p_ = p; m.n_ = len; return {std::move(m), {}};
    }
    static result<mapping> anonymous(std::size_t len, unsigned pr = prot_read | prot_write) {
        mapping m;
#if defined(FORGE_OS_WINDOWS)
        bool ex = pr & prot_exec;
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, ex ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, DWORD(std::uint64_t(len) >> 32), DWORD(len), nullptr);
        if (!h) return {{}, last_error("anonymous map")};
        void* p = MapViewOfFile(h, ((pr & prot_write) ? FILE_MAP_WRITE : FILE_MAP_READ) | (ex ? FILE_MAP_EXECUTE : 0), 0, 0, len); CloseHandle(h);
        if (!p) return {{}, last_error("anonymous map")};
        if (!(pr & (prot_read | prot_write | prot_exec))) { DWORD old; VirtualProtect(p, len, PAGE_NOACCESS, &old); }
#else
        int pp = ((pr & prot_read) ? PROT_READ : 0) | ((pr & prot_write) ? PROT_WRITE : 0) | ((pr & prot_exec) ? PROT_EXEC : 0);
        void* p = ::mmap(nullptr, len, pp, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return {{}, last_error("mmap anon")};
#endif
        m.p_ = p; m.n_ = len; return {std::move(m), {}};
    }
    status flush() {
#if defined(FORGE_OS_WINDOWS)
        if (p_ && !FlushViewOfFile(p_, n_)) return {{}, last_error("flush")};
#else
        if (p_ && ::msync(p_, n_, MS_SYNC)) return {{}, last_error("msync")};
#endif
        return {};
    }
private:
    void* p_ = nullptr; std::size_t n_ = 0;
};

// ---------------------------------------------------------------- access gate (FSS CAP-1 below forge::security)
// forge::security installs a gate while a policy is active (set_active_policy), so code that cannot depend on it
// (forge::core's spill store) can still ask before it opens a file on a user's behalf. No gate: everything is allowed.
//   if (auto s = os::check_access(os::access_kind::write, dir); !s) ...   // s.err: the policy's denial
enum class access_kind { read, write };
using access_gate = status (*)(access_kind, std::string_view path);
inline std::atomic<access_gate>& access_gate_slot() { static std::atomic<access_gate> g{nullptr}; return g; }
inline void set_access_gate(access_gate g) { access_gate_slot().store(g); }
inline status check_access(access_kind k, std::string_view path) { auto g = access_gate_slot().load(); return g ? g(k, path) : status{}; }

// ---------------------------------------------------------------- memory
inline std::size_t page_size() {
#if defined(FORGE_OS_WINDOWS)
    SYSTEM_INFO s; GetSystemInfo(&s); return s.dwPageSize;
#else
    return std::size_t(::sysconf(_SC_PAGESIZE));
#endif
}
inline void* aligned_alloc(std::size_t n, std::size_t align) {
#if defined(FORGE_OS_WINDOWS)
    return _aligned_malloc(n, align);
#else
    void* p = nullptr; return ::posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, n) ? nullptr : p;
#endif
}
inline void aligned_free(void* p) {
#if defined(FORGE_OS_WINDOWS)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
inline void* reserve(std::size_t n) {   // address space only; commit() before touching
#if defined(FORGE_OS_WINDOWS)
    return VirtualAlloc(nullptr, n, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = ::mmap(nullptr, n, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0); return p == MAP_FAILED ? nullptr : p;
#endif
}
inline bool commit(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
    return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    return ::mprotect(p, n, PROT_READ | PROT_WRITE) == 0;
#endif
}
inline void release(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
    (void)n; VirtualFree(p, 0, MEM_RELEASE);
#else
    ::munmap(p, n);
#endif
}
inline bool protect(void* p, std::size_t n, unsigned pr) {
#if defined(FORGE_OS_WINDOWS)
    DWORD f = (pr & prot_exec) ? ((pr & prot_write) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (pr & prot_write) ? PAGE_READWRITE : (pr & prot_read) ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD old; return VirtualProtect(p, n, f, &old) != 0;
#else
    int f = ((pr & prot_read) ? PROT_READ : 0) | ((pr & prot_write) ? PROT_WRITE : 0) | ((pr & prot_exec) ? PROT_EXEC : 0);
    return ::mprotect(p, n, f) == 0;
#endif
}
inline bool lock_memory(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
    return VirtualLock(p, n) != 0;
#else
    return ::mlock(p, n) == 0;
#endif
}
inline void unlock_memory(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
    VirtualUnlock(p, n);
#else
    ::munlock(p, n);
#endif
}
inline void advise_willneed(void* p, std::size_t n) {
#if defined(FORGE_OS_POSIX)
    ::madvise(p, n, MADV_WILLNEED);
#else
    WIN32_MEMORY_RANGE_ENTRY r{p, n}; PrefetchVirtualMemory(GetCurrentProcess(), 1, &r, 0);
#endif
}

// ---------------------------------------------------------------- dynamic libraries
// Plain open / symbol / close. <forge/os/dynlib.hpp> adds search dirs, typed get<>, path() and symbol enumeration.
class library {
public:
    library() = default;
    library(library&& o) noexcept : h_(std::exchange(o.h_, nullptr)), self_(std::exchange(o.self_, false)) {}
    library& operator=(library&& o) noexcept { if (this != &o) { close(); h_ = std::exchange(o.h_, nullptr); self_ = std::exchange(o.self_, false); } return *this; }
    ~library() { close(); }
    static result<library> open(const std::string& name) {   // empty name = the running program
        library l;
#if defined(FORGE_OS_WINDOWS)
        l.h_ = name.empty() ? static_cast<void*>(GetModuleHandleW(nullptr)) : static_cast<void*>(LoadLibraryW(widen(name).c_str()));
        if (!l.h_) return {{}, last_error("LoadLibrary " + name)};
#else
        l.h_ = ::dlopen(name.empty() ? nullptr : name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!l.h_) { const char* e = ::dlerror(); return {{}, {-1, e ? e : ("dlopen " + name)}}; }
#endif
        l.self_ = name.empty(); return {std::move(l), {}};
    }
    // Takes ownership of a handle from LoadLibrary / dlopen (closed by the library).
    static library adopt(void* native) { library l; l.h_ = native; return l; }
    void* symbol(const char* s) const {
        if (!h_) return nullptr;
#if defined(FORGE_OS_WINDOWS)
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(h_), s));
#else
        return ::dlsym(h_, s);
#endif
    }
    template <class F> F get(const char* s) const { return reinterpret_cast<F>(symbol(s)); }
    bool valid() const { return h_ != nullptr; }
    bool is_self() const { return self_; }   // opened with "" (the running program)
    void* native() const { return h_; }
    void close() {
        if (!h_) return;
#if defined(FORGE_OS_WINDOWS)
        if (!self_) FreeLibrary(static_cast<HMODULE>(h_));
#else
        ::dlclose(h_);
#endif
        h_ = nullptr;
    }
    // Conventional file name: "foo" -> libfoo.so / libfoo.dylib / foo.dll
    static std::string decorate(std::string_view base) {
#if defined(FORGE_OS_WINDOWS)
        return std::string(base) + ".dll";
#elif defined(__APPLE__)
        return "lib" + std::string(base) + ".dylib";
#else
        return "lib" + std::string(base) + ".so";
#endif
    }
private:
    void* h_ = nullptr; bool self_ = false;
};

// ---------------------------------------------------------------- environment
inline std::string getenv(const std::string& k, const std::string& dflt) {
#if defined(FORGE_OS_WINDOWS)
    wchar_t b[32767]; DWORD n = GetEnvironmentVariableW(widen(k).c_str(), b, 32767); return n ? narrow({b, n}) : dflt;
#else
    const char* v = std::getenv(k.c_str()); return v ? v : dflt;
#endif
}
inline bool has_env(const std::string& k) {
#if defined(FORGE_OS_WINDOWS)
    return GetEnvironmentVariableW(widen(k).c_str(), nullptr, 0) != 0;
#else
    return std::getenv(k.c_str()) != nullptr;
#endif
}
inline bool setenv(const std::string& k, const std::string& v) {
#if defined(FORGE_OS_WINDOWS)
    return SetEnvironmentVariableW(widen(k).c_str(), widen(v).c_str()) && _putenv_s(k.c_str(), v.c_str()) == 0;
#else
    return ::setenv(k.c_str(), v.c_str(), 1) == 0;
#endif
}
inline bool unsetenv(const std::string& k) {
#if defined(FORGE_OS_WINDOWS)
    return SetEnvironmentVariableW(widen(k).c_str(), nullptr) && _putenv_s(k.c_str(), "") == 0;
#else
    return ::unsetenv(k.c_str()) == 0;
#endif
}

// ---------------------------------------------------------------- time
inline std::uint64_t monotonic_ns() {
#if defined(FORGE_OS_WINDOWS)
    static const std::uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return std::uint64_t(q.QuadPart); }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c); std::uint64_t t = std::uint64_t(c.QuadPart);
    return t / f * 1000000000ull + t % f * 1000000000ull / f;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts); return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
#endif
}
inline std::int64_t wall_ns() {   // Unix epoch
#if defined(FORGE_OS_WINDOWS)
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    return (std::int64_t((std::uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000LL) * 100;
#else
    timespec ts; ::clock_gettime(CLOCK_REALTIME, &ts); return std::int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
}
inline std::uint64_t cpu_time_ns() {   // this process, user + system
#if defined(FORGE_OS_WINDOWS)
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) * 100;
#else
    timespec ts; ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
#endif
}
inline std::uint64_t thread_cpu_time_ns() {   // the calling thread, user + system
#if defined(FORGE_OS_WINDOWS)
    FILETIME c, e, k, u; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) * 100;
#else
    timespec ts; ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts); return std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec);
#endif
}
inline void sleep_ns(std::uint64_t ns) {
#if defined(FORGE_OS_WINDOWS)
    // A high-resolution waitable timer (Windows 10 1803+) gives sub-millisecond sleeps instead of Sleep()'s 1 ms
    // floor and ~15.6 ms tick; one per thread. Older systems fall back to Sleep in chunks below INFINITE.
    struct timer { HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS); ~timer() { if (h) CloseHandle(h); } };
    static thread_local timer t;
    if (t.h && ns) {
        LARGE_INTEGER due; due.QuadPart = -LONGLONG(ns / 100 < 0x7fffffffffffffffull ? (ns + 99) / 100 : 0x7fffffffffffffffull);   // relative, 100 ns units
        if (SetWaitableTimer(t.h, &due, 0, nullptr, nullptr, FALSE) && WaitForSingleObject(t.h, INFINITE) == WAIT_OBJECT_0) return;
    }
    std::uint64_t ms = (ns + 999999) / 1000000;
    do { DWORD c = DWORD(ms < 0xFFFFFFFEull ? ms : 0xFFFFFFFEull); Sleep(c); ms -= c; } while (ms);
#else
    timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)}; while (::nanosleep(&ts, &ts) && errno == EINTR) {}
#endif
}

// ---------------------------------------------------------------- threads
inline std::uint64_t thread_id() {
#if defined(FORGE_OS_WINDOWS)
    return GetCurrentThreadId();
#elif defined(__APPLE__)
    std::uint64_t t = 0; pthread_threadid_np(nullptr, &t); return t;
#elif defined(FORGE_OS_FREEBSD)
    return std::uint64_t(pthread_getthreadid_np());
#else
    return std::uint64_t(::gettid());
#endif
}
inline void yield() {
#if defined(FORGE_OS_WINDOWS)
    SwitchToThread();
#else
    ::sched_yield();
#endif
}
inline void set_thread_name(const std::string& n) {
#if defined(FORGE_OS_WINDOWS)
    using fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    if (auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")))) f(GetCurrentThread(), widen(n).c_str());
#elif defined(__APPLE__)
    pthread_setname_np(n.substr(0, 63).c_str());
#elif defined(FORGE_OS_FREEBSD)
    pthread_set_name_np(pthread_self(), n.c_str());
#else
    pthread_setname_np(pthread_self(), n.substr(0, 15).c_str());
#endif
}
enum class thread_priority { low, normal, high };
inline bool set_thread_priority(thread_priority p) {
#if defined(FORGE_OS_WINDOWS)
    return SetThreadPriority(GetCurrentThread(), p == thread_priority::low ? THREAD_PRIORITY_BELOW_NORMAL : p == thread_priority::high ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL) != 0;
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(p == thread_priority::low ? QOS_CLASS_UTILITY : p == thread_priority::high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0) == 0;
#elif defined(FORGE_OS_FREEBSD)
    // setpriority(PRIO_PROCESS, 0) renices the whole process here; rtprio_thread sets the calling thread's base
    // user priority within the time-sharing class: high / normal / low = 0 / RTP_PRIO_MAX/2 / RTP_PRIO_MAX (ULE
    // still adjusts it for interactivity).
    struct rtprio r{}; r.type = RTP_PRIO_NORMAL;
    r.prio = p == thread_priority::low ? RTP_PRIO_MAX : p == thread_priority::high ? 0 : RTP_PRIO_MAX / 2;
    return ::rtprio_thread(RTP_SET, 0, &r) == 0;
#else
    // Nice level of the calling thread (Linux/Android nice values are per-thread). Raising priority needs
    // CAP_SYS_NICE or RLIMIT_NICE, so `high` usually fails for ordinary processes and reports it.
    return ::setpriority(PRIO_PROCESS, id_t(::gettid()), p == thread_priority::low ? 10 : p == thread_priority::high ? -5 : 0) == 0;
#endif
}
inline bool pin_thread(unsigned cpu) {
#if defined(FORGE_OS_WINDOWS)
    // cpu counts across processor groups (64 per group), in group order.
    WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; ++g) {
        DWORD n = GetActiveProcessorCount(g);
        if (cpu < n) { GROUP_AFFINITY a{}; a.Group = g; a.Mask = KAFFINITY(1) << cpu; return SetThreadGroupAffinity(GetCurrentThread(), &a, nullptr) != 0; }
        cpu -= n;
    }
    return false;
#elif defined(FORGE_OS_LINUX) || defined(FORGE_OS_ANDROID)
    if (cpu >= CPU_SETSIZE) return false;
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); return ::sched_setaffinity(0, sizeof s, &s) == 0;
#elif defined(FORGE_OS_FREEBSD)
    if (cpu >= CPU_SETSIZE) return false;
    cpuset_t s; CPU_ZERO(&s); CPU_SET(cpu, &s); return ::cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof s, &s) == 0;
#else
    (void)cpu; return false;   // macOS/iOS have no hard affinity
#endif
}
class thread {
public:
    thread() = default;
    explicit thread(std::function<void()> f, std::string name = {}) {
        auto* st = new start{std::move(f), std::move(name)};
#if defined(FORGE_OS_WINDOWS)
        h_ = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &entry, st, 0, nullptr));
#else
        if (pthread_create(&t_, nullptr, &entry, st) == 0) live_ = true;
        else delete st;
#endif
    }
    thread(thread&& o) noexcept { *this = std::move(o); }
    thread& operator=(thread&& o) noexcept {   // a running thread being replaced is joined first (never leaked)
        if (this == &o) return *this;
        join();
#if defined(FORGE_OS_WINDOWS)
        h_ = std::exchange(o.h_, nullptr);
#else
        t_ = o.t_; live_ = std::exchange(o.live_, false);
#endif
        return *this;
    }
    ~thread() { join(); }
    bool joinable() const {
#if defined(FORGE_OS_WINDOWS)
        return h_ != nullptr;
#else
        return live_;
#endif
    }
    void join() {
#if defined(FORGE_OS_WINDOWS)
        if (h_) { WaitForSingleObject(h_, INFINITE); CloseHandle(h_); h_ = nullptr; }
#else
        if (live_) { pthread_join(t_, nullptr); live_ = false; }
#endif
    }
private:
    struct start { std::function<void()> f; std::string name; };
#if defined(FORGE_OS_WINDOWS)
    static unsigned __stdcall entry(void* p) { auto* s = static_cast<start*>(p); if (!s->name.empty()) set_thread_name(s->name); s->f(); delete s; return 0; }
    HANDLE h_ = nullptr;
#else
    static void* entry(void* p) { auto* s = static_cast<start*>(p); if (!s->name.empty()) set_thread_name(s->name); s->f(); delete s; return nullptr; }
    pthread_t t_{}; bool live_ = false;
#endif
};

// ---------------------------------------------------------------- sync
class mutex {
public:
    mutex() {
#if defined(FORGE_OS_WINDOWS)
        InitializeSRWLock(&m_);
#else
        pthread_mutex_init(&m_, nullptr);
#endif
    }
    ~mutex() {
#if defined(FORGE_OS_POSIX)
        pthread_mutex_destroy(&m_);
#endif
    }
    mutex(const mutex&) = delete; mutex& operator=(const mutex&) = delete;
#if defined(FORGE_OS_WINDOWS)
    void lock() { AcquireSRWLockExclusive(&m_); }
    bool try_lock() { return TryAcquireSRWLockExclusive(&m_); }
    void unlock() { ReleaseSRWLockExclusive(&m_); }
    SRWLOCK m_;
#else
    void lock() { pthread_mutex_lock(&m_); }
    bool try_lock() { return pthread_mutex_trylock(&m_) == 0; }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t m_;
#endif
};
class condvar {
public:
    condvar() {
#if defined(FORGE_OS_WINDOWS)
        InitializeConditionVariable(&c_);
#elif defined(__APPLE__)
        pthread_cond_init(&c_, nullptr);
#else
        pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&c_, &a); pthread_condattr_destroy(&a);
#endif
    }
    ~condvar() {
#if defined(FORGE_OS_POSIX)
        pthread_cond_destroy(&c_);
#endif
    }
    condvar(const condvar&) = delete; condvar& operator=(const condvar&) = delete;
    void notify_one() {
#if defined(FORGE_OS_WINDOWS)
        WakeConditionVariable(&c_);
#else
        pthread_cond_signal(&c_);
#endif
    }
    void notify_all() {
#if defined(FORGE_OS_WINDOWS)
        WakeAllConditionVariable(&c_);
#else
        pthread_cond_broadcast(&c_);
#endif
    }
    void wait(mutex& m) {
#if defined(FORGE_OS_WINDOWS)
        SleepConditionVariableSRW(&c_, &m.m_, INFINITE, 0);
#else
        pthread_cond_wait(&c_, &m.m_);
#endif
    }
    // false on timeout
    bool wait_for(mutex& m, std::uint64_t ns) {
        // Windows waits in whole milliseconds (rounded up); waits beyond ~49 days and ~292 years are capped.
        if (ns > (1ull << 62)) ns = 1ull << 62;
#if defined(FORGE_OS_WINDOWS)
        std::uint64_t ms = (ns + 999999) / 1000000;
        return SleepConditionVariableSRW(&c_, &m.m_, DWORD(ms < 0xFFFFFFFEull ? ms : 0xFFFFFFFEull), 0) != 0;
#elif defined(__APPLE__)
        timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
        return pthread_cond_timedwait_relative_np(&c_, &m.m_, &ts) == 0;
#else
        timespec ts; ::clock_gettime(CLOCK_MONOTONIC, &ts);
        std::uint64_t t = std::uint64_t(ts.tv_sec) * 1000000000ull + std::uint64_t(ts.tv_nsec) + ns;
        ts.tv_sec = time_t(t / 1000000000ull); ts.tv_nsec = long(t % 1000000000ull);
        return pthread_cond_timedwait(&c_, &m.m_, &ts) == 0;
#endif
    }
private:
#if defined(FORGE_OS_WINDOWS)
    CONDITION_VARIABLE c_;
#else
    pthread_cond_t c_;
#endif
};
struct lock_guard { mutex& m; explicit lock_guard(mutex& x) : m(x) { m.lock(); } ~lock_guard() { m.unlock(); } };
class semaphore {
public:
    explicit semaphore(unsigned initial = 0) : n_(initial) {}
    void post(unsigned k = 1) { lock_guard g(m_); n_ += k; if (k == 1) c_.notify_one(); else c_.notify_all(); }
    void wait() { lock_guard g(m_); while (!n_) c_.wait(m_); --n_; }
    bool wait_for(std::uint64_t ns) {
        std::uint64_t end = monotonic_ns() + ns; lock_guard g(m_);
        while (!n_) { auto now = monotonic_ns(); if (now >= end || (!c_.wait_for(m_, end - now) && !n_ && monotonic_ns() >= end)) return false; }
        --n_; return true;
    }
private:
    mutex m_; condvar c_; unsigned n_;
};

// ---------------------------------------------------------------- processes
inline std::uint64_t pid() {
#if defined(FORGE_OS_WINDOWS)
    return GetCurrentProcessId();
#else
    return std::uint64_t(::getpid());
#endif
}
struct exec_result { int exit_code = -1; std::string out; };
// Run argv[0] with args, capture stdout (+ stderr when merge_stderr), wait. iOS cannot spawn processes.
inline result<exec_result> run(const std::vector<std::string>& argv, bool merge_stderr = false) {
    if (argv.empty()) return {{}, {-1, "run: empty argv"}};
#if defined(FORGE_OS_IOS)
    (void)merge_stderr; return {{}, {-1, "run: process spawning is not available on iOS"}};
#elif defined(FORGE_OS_WINDOWS)
    std::wstring cmd;
    for (auto& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        std::wstring w = widen(a);
        if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) { cmd += w; continue; }
        cmd += L'"'; std::size_t bs = 0;
        for (wchar_t c : w) { if (c == L'\\') { ++bs; continue; } if (c == L'"') cmd.append(bs * 2 + 1, L'\\'); else cmd.append(bs, L'\\'); bs = 0; cmd += c; }
        cmd.append(bs * 2, L'\\'); cmd += L'"';
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE}; HANDLE r, w;
    if (!CreatePipe(&r, &w, &sa, 0)) return {{}, last_error("CreatePipe")};
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = w; si.hStdError = merge_stderr ? w : GetStdHandle(STD_ERROR_HANDLE); si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = create_process_std_only(nullptr, cmd.data(), CREATE_NO_WINDOW, nullptr, nullptr, si, pi);
    CloseHandle(w);
    if (!ok) { auto e = last_error("CreateProcess " + argv[0]); CloseHandle(r); return {{}, e}; }
    exec_result res; char buf[4096]; DWORD n;
    while (ReadFile(r, buf, sizeof buf, &n, nullptr) && n) res.out.append(buf, n);
    CloseHandle(r); WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); res.exit_code = int(code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return {std::move(res), {}};
#else
    int fds[2]; if (cloexec_pipe(fds)) return {{}, last_error("pipe")};   // close-on-exec: other threads' children must not inherit it
    std::vector<char*> av; for (auto& a : argv) av.push_back(const_cast<char*>(a.c_str())); av.push_back(nullptr);
    pid_t p; int rc = 0;
#if defined(FORGE_OS_ANDROID) && __ANDROID_API__ < 28   // posix_spawn arrives in API 28
    p = serialized_fork();
    if (p == 0) { ::dup2(fds[1], 1); if (merge_stderr) ::dup2(fds[1], 2); ::close(fds[0]); ::close(fds[1]); ::execvp(av[0], av.data()); ::_exit(127); }
    if (p < 0) rc = errno;
    ::close(fds[1]);
#else
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    if (merge_stderr) posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    // dup2 clears close-on-exec on the child's fd 1/2; both pipe ends close at exec on their own. (No addclose:
    // when fds[1] lands on 1 or 2 the close would undo the dup.)
    rc = ::posix_spawnp(&p, av[0], &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa); ::close(fds[1]);
#endif
    if (rc) { ::close(fds[0]); return {{}, {rc, "spawn " + argv[0] + ": " + error_text(rc)}}; }
    exec_result res; char buf[4096];
    for (;;) { auto n = ::read(fds[0], buf, sizeof buf); if (n > 0) res.out.append(buf, std::size_t(n)); else if (n == 0 || errno != EINTR) break; }
    ::close(fds[0]);
    int st = 0; while (::waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    res.exit_code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return {std::move(res), {}};
#endif
}

// ---------------------------------------------------------------- signals
enum class sig { interrupt, terminate, hangup, pipe };
// Install a handler for Ctrl-C / termination. The handler runs in signal context (POSIX) or on a
// console-control thread (Windows): keep it to setting atomics.
inline bool on_signal(sig s, void (*handler)(int)) {
#if defined(FORGE_OS_WINDOWS)
    // Ctrl-C -> interrupt (2); Ctrl-Break / shutdown -> terminate (15); console window closed / user logoff ->
    // hangup (1), or terminate when no hangup handler is set. SIGPIPE does not exist (writes fail with an error).
    static std::atomic<void (*)(int)> h_int{nullptr}, h_term{nullptr}, h_hup{nullptr};
    if (s == sig::interrupt) h_int = handler; else if (s == sig::terminate) h_term = handler; else if (s == sig::hangup) h_hup = handler; else return true;
    static const bool installed = SetConsoleCtrlHandler([](DWORD t) -> BOOL {
        void (*hi)(int) = h_int, (*ht)(int) = h_term, (*hh)(int) = h_hup;
        if (t == CTRL_C_EVENT && hi) { hi(2); return TRUE; }
        if ((t == CTRL_CLOSE_EVENT || t == CTRL_LOGOFF_EVENT) && hh) { hh(1); return TRUE; }
        if ((t == CTRL_CLOSE_EVENT || t == CTRL_BREAK_EVENT || t == CTRL_SHUTDOWN_EVENT) && ht) { ht(15); return TRUE; }
        return FALSE; }, TRUE) != 0;
    return installed;
#else
    int n = s == sig::interrupt ? SIGINT : s == sig::terminate ? SIGTERM : s == sig::hangup ? SIGHUP : SIGPIPE;
    struct sigaction a{}; a.sa_handler = handler ? handler : SIG_DFL; sigemptyset(&a.sa_mask); a.sa_flags = SA_RESTART;
    return ::sigaction(n, &a, nullptr) == 0;
#endif
}
inline void ignore_sigpipe() {
#if defined(FORGE_OS_POSIX)
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

// ---------------------------------------------------------------- system / CPU info
struct system_info {
    std::string os, arch, hostname;
    unsigned logical_cpus = 1;
    std::uint64_t total_ram = 0, avail_ram = 0;
    std::size_t page = 4096;
};
inline unsigned cpu_count() {
#if defined(FORGE_OS_WINDOWS)
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS); return n ? n : 1;
#elif defined(FORGE_OS_LINUX) || defined(FORGE_OS_ANDROID)
    cpu_set_t s; if (::sched_getaffinity(0, sizeof s, &s) == 0) { int n = CPU_COUNT(&s); if (n > 0) return unsigned(n); }
    long n = ::sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? unsigned(n) : 1;
#else
    long n = ::sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? unsigned(n) : 1;
#endif
}
inline std::uint64_t total_memory() {
#if defined(FORGE_OS_WINDOWS)
    MEMORYSTATUSEX m{}; m.dwLength = sizeof m; GlobalMemoryStatusEx(&m); return m.ullTotalPhys;
#elif defined(__APPLE__)
    std::uint64_t v = 0; std::size_t n = sizeof v; ::sysctlbyname("hw.memsize", &v, &n, nullptr, 0); return v;
#elif defined(FORGE_OS_FREEBSD)
    std::uint64_t v = 0; std::size_t n = sizeof v; ::sysctlbyname("hw.physmem", &v, &n, nullptr, 0); return v;
#else
    return std::uint64_t(::sysconf(_SC_PHYS_PAGES)) * std::uint64_t(::sysconf(_SC_PAGESIZE));
#endif
}
inline std::uint64_t available_memory() {
#if defined(FORGE_OS_WINDOWS)
    MEMORYSTATUSEX m{}; m.dwLength = sizeof m; GlobalMemoryStatusEx(&m); return m.ullAvailPhys;
#elif defined(FORGE_OS_LINUX) || defined(FORGE_OS_ANDROID)
    if (auto s = read_file("/proc/meminfo")) { auto p = s->find("MemAvailable:"); if (p != std::string::npos) return std::strtoull(s->c_str() + p + 13, nullptr, 10) * 1024; }
    return std::uint64_t(::sysconf(_SC_AVPHYS_PAGES)) * std::uint64_t(::sysconf(_SC_PAGESIZE));
#elif defined(__APPLE__)
#if defined(FORGE_OS_IOS)
    // iOS 13+: what this app may still allocate before jetsam (the per-app limit, not the device's free RAM).
    using avail_fn = std::size_t (*)();
    if (auto f = reinterpret_cast<avail_fn>(::dlsym(RTLD_DEFAULT, "os_proc_available_memory"))) if (std::size_t a = f()) return a;
#endif
    // Free + inactive + speculative pages: what the VM can hand out without paging anything to disk. (Purgeable
    // pages live inside the active/inactive counts, so adding them would count twice.)
    vm_statistics64_data_t vs{}; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT; mach_port_t host = mach_host_self();
    kern_return_t kr = host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vs), &cnt);
    mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) return 0;
    std::uint64_t a = (std::uint64_t(vs.free_count) + vs.inactive_count + vs.speculative_count) * std::uint64_t(vm_kernel_page_size), t = total_memory();
    return t && a > t ? t : a;
#else
    // Free + inactive (clean, reclaimable without I/O). Laundry pages need a write first and are not counted.
    unsigned f = 0, in = 0; std::size_t n = sizeof f; ::sysctlbyname("vm.stats.vm.v_free_count", &f, &n, nullptr, 0);
    n = sizeof in; ::sysctlbyname("vm.stats.vm.v_inactive_count", &in, &n, nullptr, 0);
    return (std::uint64_t(f) + in) * page_size();
#endif
}
inline std::string hostname() {
#if defined(FORGE_OS_WINDOWS)
    wchar_t b[256]; DWORD n = 256; return GetComputerNameExW(ComputerNameDnsHostname, b, &n) ? narrow({b, n}) : "";
#else
    char b[256] = {}; ::gethostname(b, sizeof b - 1); return b;
#endif
}
constexpr const char* arch_name() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__riscv) && __riscv_xlen == 64
    return "riscv64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}
inline system_info sysinfo() {
    return {platform_name(), arch_name(), hostname(), cpu_count(), total_memory(), available_memory(), page_size()};
}

// ---------------------------------------------------------------- entropy
inline bool entropy(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(p), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__) || defined(FORGE_OS_ANDROID) || defined(FORGE_OS_FREEBSD)
    ::arc4random_buf(p, n); return true;
#else
    auto* b = static_cast<std::uint8_t*>(p);
    while (n) { auto r = ::getrandom(b, n, 0); if (r < 0 && errno == EINTR) continue; if (r <= 0) return false; b += r; n -= std::size_t(r); }
    return true;
#endif
}

// ---------------------------------------------------------------- sockets
#if defined(FORGE_OS_WINDOWS)
using sock_t = SOCKET;
inline const sock_t bad_sock = INVALID_SOCKET;
#else
using sock_t = int;
inline constexpr sock_t bad_sock = -1;
#endif
inline void net_init() {
#if defined(FORGE_OS_WINDOWS)
    static bool ok = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }(); (void)ok;
#endif
}
inline int sock_errno() {
#if defined(FORGE_OS_WINDOWS)
    return WSAGetLastError();
#else
    return errno;
#endif
}
inline bool would_block(int e = sock_errno()) {
#if defined(FORGE_OS_WINDOWS)
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS;
#endif
}
class socket {
public:
    socket() = default;
    explicit socket(sock_t s) : s_(s) {}
    socket(socket&& o) noexcept : s_(std::exchange(o.s_, bad_sock)) {}
    socket& operator=(socket&& o) noexcept { if (this != &o) { close(); s_ = std::exchange(o.s_, bad_sock); } return *this; }
    ~socket() { close(); }
    sock_t native() const { return s_; }
    sock_t release() { return std::exchange(s_, bad_sock); }
    bool valid() const { return s_ != bad_sock; }
    void close() {
        if (!valid()) return;
#if defined(FORGE_OS_WINDOWS)
        ::closesocket(s_);
#else
        ::close(s_);
#endif
        s_ = bad_sock;
    }
    bool set_nonblocking(bool on = true) {
#if defined(FORGE_OS_WINDOWS)
        u_long v = on; return ::ioctlsocket(s_, FIONBIO, &v) == 0;
#else
        int f = ::fcntl(s_, F_GETFL); return f >= 0 && ::fcntl(s_, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK)) == 0;
#endif
    }
    bool set_nodelay(bool on = true) { int v = on; return ::setsockopt(s_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&v), sizeof v) == 0; }
    // POSIX SO_REUSEADDR: rebind a port in TIME_WAIT. Windows' SO_REUSEADDR instead lets another socket steal a
    // port that is actively listening, and Windows rebinds TIME_WAIT ports anyway, so there this sets
    // SO_EXCLUSIVEADDRUSE (nobody else can bind the port while we hold it).
    bool set_reuseaddr(bool on = true) {
        int v = on;
#if defined(FORGE_OS_WINDOWS)
        return ::setsockopt(s_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&v), sizeof v) == 0;
#else
        return ::setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&v), sizeof v) == 0;
#endif
    }
    bool set_timeout_ms(unsigned ms) {
#if defined(FORGE_OS_WINDOWS)
        DWORD v = ms;
#else
        timeval v{time_t(ms / 1000), suseconds_t((ms % 1000) * 1000)};
#endif
        return ::setsockopt(s_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&v), sizeof v) == 0 &&
               ::setsockopt(s_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&v), sizeof v) == 0;
    }
    // >0 bytes, 0 orderly close, <0 error (check would_block())
    long send(const void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
        return ::send(s_, static_cast<const char*>(p), int(n), 0);
#elif defined(MSG_NOSIGNAL)
        return long(::send(s_, p, n, MSG_NOSIGNAL));
#else
        return long(::send(s_, p, n, 0));   // Apple: SO_NOSIGPIPE set at creation
#endif
    }
    long recv(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
        return ::recv(s_, static_cast<char*>(p), int(n), 0);
#else
        for (;;) { auto r = ::recv(s_, p, n, 0); if (r >= 0 || errno != EINTR) return long(r); }
#endif
    }
    bool send_all(const void* p, std::size_t n) {
        auto* b = static_cast<const char*>(p);
        while (n) { long r = send(b, n); if (r <= 0) { if (r < 0 && would_block()) { wait_writable(1000); continue; } return false; } b += r; n -= std::size_t(r); }
        return true;
    }
    bool recv_all(void* p, std::size_t n) {
        auto* b = static_cast<char*>(p);
        while (n) { long r = recv(b, n); if (r <= 0) { if (r < 0 && would_block()) { wait_readable(1000); continue; } return false; } b += r; n -= std::size_t(r); }
        return true;
    }
    bool wait_readable(int ms) const { return poll1(POLLIN, ms); }
    bool wait_writable(int ms) const { return poll1(POLLOUT, ms); }
    int local_port() const {
        sockaddr_storage a{}; socklen_t l = sizeof a;
        if (::getsockname(s_, reinterpret_cast<sockaddr*>(&a), &l)) return -1;
        return a.ss_family == AF_INET6 ? ntohs(reinterpret_cast<sockaddr_in6*>(&a)->sin6_port) : ntohs(reinterpret_cast<sockaddr_in*>(&a)->sin_port);
    }
    // Sockets are created close-on-exec (POSIX) / non-inheritable (Windows), so children spawned by other threads
    // do not keep them open.
    static socket make(int family, int type) {
        net_init();
#if defined(FORGE_OS_WINDOWS)
        sock_t s = ::WSASocketW(family, type, 0, nullptr, 0, WSA_FLAG_OVERLAPPED | 0x80 /*WSA_FLAG_NO_HANDLE_INHERIT*/);
        if (s == bad_sock && ::WSAGetLastError() == WSAEINVAL) {   // before Windows 7 SP1: no NO_HANDLE_INHERIT
            s = ::WSASocketW(family, type, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
            if (s != bad_sock) SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
        }
#else
        sock_t s = cloexec_socket(family, type);
#endif
#if defined(__APPLE__)
        if (s != bad_sock) { int one = 1; ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
        return socket(s);
    }
    // TCP listen on host:port (port 0 = ephemeral). host "" = all interfaces.
    static result<socket> listen(const std::string& host, int port, int backlog = 128) {
        auto ai = resolve(host.empty() ? nullptr : host.c_str(), port, true); if (!ai) return {{}, ai.err};
        int last = 0;
        for (auto* a = ai.value; a; a = a->ai_next) {
            socket s = make(a->ai_family, SOCK_STREAM); if (!s.valid()) { last = sock_errno(); continue; }
            s.set_reuseaddr();
            if (::bind(s.s_, a->ai_addr, socklen_t(a->ai_addrlen)) == 0 && ::listen(s.s_, backlog) == 0) { ::freeaddrinfo(ai.value); return {std::move(s), {}}; }
            last = sock_errno();
        }
        ::freeaddrinfo(ai.value);
        return {{}, {last ? last : -1, "listen " + host + ":" + std::to_string(port) + ": " + (last ? error_text(last) : std::string("no address"))}};
    }
    static result<socket> connect(const std::string& host, int port, int timeout_ms = 5000) {
        auto ai = resolve(host.c_str(), port, false); if (!ai) return {{}, ai.err};
        int last = 0;   // the reason the last address failed: refused, unreachable, timed out...
        for (auto* a = ai.value; a; a = a->ai_next) {
            socket s = make(a->ai_family, SOCK_STREAM); if (!s.valid()) { last = sock_errno(); continue; }
            s.set_nonblocking(true);
            if (::connect(s.s_, a->ai_addr, socklen_t(a->ai_addrlen)) != 0) {
                int e = sock_errno();
                if (!would_block(e)) { last = e; continue; }
                if (!s.wait_writable(timeout_ms)) {
#if defined(FORGE_OS_WINDOWS)
                    last = WSAETIMEDOUT;
#else
                    last = ETIMEDOUT;
#endif
                    continue;
                }
                if (int so = s.so_error()) { last = so; continue; }
            }
            s.set_nonblocking(false); s.set_nodelay();
            ::freeaddrinfo(ai.value); return {std::move(s), {}};
        }
        ::freeaddrinfo(ai.value);
        return {{}, {last ? last : -1, "connect " + host + ":" + std::to_string(port) + ": " + (last ? error_text(last) : std::string("no address"))}};
    }
    result<socket> accept() {
        for (;;) {
#if defined(FORGE_OS_WINDOWS)
            sock_t c = ::accept(s_, nullptr, nullptr);
            if (c != bad_sock) SetHandleInformation(reinterpret_cast<HANDLE>(c), HANDLE_FLAG_INHERIT, 0);
#else
            sock_t c = cloexec_accept(s_);
#endif
            if (c != bad_sock) {
#if defined(__APPLE__)
                int one = 1; ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                socket s(c); s.set_nodelay(); return {std::move(s), {}};
            }
            int e = sock_errno(); return {{}, {e, "accept: " + error_text(e)}};
        }
    }
    int so_error() const { int e = 0; socklen_t l = sizeof e; ::getsockopt(s_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&e), &l); return e; }
    void shutdown_both() {
#if defined(FORGE_OS_WINDOWS)
        ::shutdown(s_, SD_BOTH);
#else
        ::shutdown(s_, SHUT_RDWR);
#endif
    }
    void shutdown_write() {
#if defined(FORGE_OS_WINDOWS)
        ::shutdown(s_, SD_SEND);
#else
        ::shutdown(s_, SHUT_WR);
#endif
    }
    static result<addrinfo*> resolve(const char* host, int port, bool passive) {
        net_init();
        addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM; if (passive) hints.ai_flags = AI_PASSIVE;
        addrinfo* r = nullptr; std::string ps = std::to_string(port);
        if (int e = ::getaddrinfo(host, ps.c_str(), &hints, &r); e) {
#if defined(EAI_SYSTEM)
            if (e == EAI_SYSTEM && errno) { int c = errno; return {nullptr, errno_error(c, std::string("resolve ") + (host ? host : "*") + ": " + error_text(c))}; }
#endif
            return {nullptr, {e, std::string("resolve ") + (host ? host : "*") + ": " + ::gai_strerror(e), err_domain::gai}};
        }
        return {r, {}};
    }
private:
    bool poll1(short ev, int ms) const {
#if defined(FORGE_OS_WINDOWS)
        WSAPOLLFD p{s_, ev, 0}; return ::WSAPoll(&p, 1, ms) > 0;
#else
        pollfd p{s_, ev, 0}; for (;;) { int r = ::poll(&p, 1, ms); if (r >= 0) return r > 0; if (errno != EINTR) return false; }
#endif
    }
    sock_t s_ = bad_sock;
};

// ---------------------------------------------------------------- readiness polling
// epoll (Linux/Android), kqueue (macOS/iOS/FreeBSD), WSAPoll (Windows). Level-triggered.
enum poll_events : unsigned { readable = 1, writable = 2, hangup = 4 };
struct poll_event { sock_t fd; unsigned events; void* user; };
class poller {
public:
    poller() {
#if defined(FORGE_OS_EPOLL)
        q_ = ::epoll_create1(EPOLL_CLOEXEC);
#elif defined(FORGE_OS_KQUEUE)
        q_ = ::kqueue();
#endif
    }
    ~poller() {
        clear();
#if defined(FORGE_OS_EPOLL) || defined(FORGE_OS_KQUEUE)
        if (q_ >= 0) ::close(q_);
#endif
    }
    poller(const poller&) = delete; poller& operator=(const poller&) = delete;
    const char* backend() const {
#if defined(FORGE_OS_EPOLL)
        return "epoll";
#elif defined(FORGE_OS_KQUEUE)
        return "kqueue";
#elif defined(FORGE_OS_WINDOWS)
        return "wsapoll";
#else
        return "poll";
#endif
    }
    bool add(sock_t fd, unsigned ev, void* user = nullptr) { return ctl(fd, ev, user, 0); }
    bool modify(sock_t fd, unsigned ev, void* user = nullptr) { return ctl(fd, ev, user, 1); }
    bool remove(sock_t fd) { return ctl(fd, 0, nullptr, 2); }
    // Waits up to timeout_ms (-1 = forever). Returns the ready events; empty on timeout, on a signal (EINTR) and
    // on failure. The overload with `err` tells a failure apart (err set) from a timeout or signal (err clear).
    std::vector<poll_event> wait(int timeout_ms) { error e; return wait(timeout_ms, e); }
    std::vector<poll_event> wait(int timeout_ms, error& err) {
        std::vector<poll_event> out; err = {};
        auto failed = [&](int n) {
            if (n >= 0) return false;
#if defined(FORGE_OS_WINDOWS)
            err = {sock_errno(), "poll: " + error_text(sock_errno())};
#else
            if (errno != EINTR) err = last_error("poll");
#endif
            return true;
        };
#if defined(FORGE_OS_EPOLL)
        if (q_ < 0) { err = {EBADF, "poll: no epoll instance"}; return out; }
        epoll_event ev[64]; int n = ::epoll_wait(q_, ev, 64, timeout_ms); if (failed(n)) return out;
        for (int i = 0; i < n; ++i) {
            auto* r = static_cast<reg*>(ev[i].data.ptr);
            unsigned e = ((ev[i].events & EPOLLIN) ? readable : 0) | ((ev[i].events & EPOLLOUT) ? writable : 0) | ((ev[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) ? hangup : 0);
            out.push_back({r->fd, e, r->user});
        }
#elif defined(FORGE_OS_KQUEUE)
        struct kevent ev[64]; timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        if (q_ < 0) { err = {EBADF, "poll: no kqueue"}; return out; }
        int n = ::kevent(q_, nullptr, 0, ev, 64, timeout_ms < 0 ? nullptr : &ts); if (failed(n)) return out;
        for (int i = 0; i < n; ++i) {
            auto* r = static_cast<reg*>(ev[i].udata);
            unsigned e = (ev[i].filter == EVFILT_READ ? readable : 0) | (ev[i].filter == EVFILT_WRITE ? writable : 0) | ((ev[i].flags & (EV_EOF | EV_ERROR)) ? hangup : 0);
            bool merged = false;
            for (auto& o : out) if (o.fd == r->fd) { o.events |= e; merged = true; }
            if (!merged) out.push_back({r->fd, e, r->user});
        }
#else
        std::vector<
#if defined(FORGE_OS_WINDOWS)
            WSAPOLLFD
#else
            pollfd
#endif
            > p;
        for (auto& r : regs_) p.push_back({r->fd, short(((r->ev & readable) ? POLLIN : 0) | ((r->ev & writable) ? POLLOUT : 0)), 0});
        if (p.empty()) { sleep_ns(std::uint64_t(timeout_ms < 0 ? 1000 : timeout_ms) * 1000000ull); return out; }
#if defined(FORGE_OS_WINDOWS)
        int n = ::WSAPoll(p.data(), ULONG(p.size()), timeout_ms);
#else
        int n = ::poll(p.data(), nfds_t(p.size()), timeout_ms);
#endif
        if (failed(n)) return out;
        for (std::size_t i = 0; n > 0 && i < p.size(); ++i)
            if (p[i].revents) {
                unsigned e = ((p[i].revents & POLLIN) ? readable : 0) | ((p[i].revents & POLLOUT) ? writable : 0) | ((p[i].revents & (POLLHUP | POLLERR)) ? hangup : 0);
                out.push_back({p[i].fd, e, regs_[i]->user});
            }
#endif
        return out;
    }
    std::size_t size() const { return regs_.size(); }
private:
    struct reg { sock_t fd; unsigned ev; void* user; };
    reg* find(sock_t fd) { for (auto& r : regs_) if (r->fd == fd) return r; return nullptr; }
    bool ctl(sock_t fd, unsigned ev, void* user, int op) {
        reg* r = find(fd);
        if (op == 0) { if (r) return false; regs_.push_back(new reg{fd, ev, user}); r = regs_.back(); }
        else if (!r) return false;
        bool ok = true;
#if defined(FORGE_OS_EPOLL)
        epoll_event e{}; e.events = ((ev & readable) ? EPOLLIN : 0) | ((ev & writable) ? EPOLLOUT : 0) | EPOLLRDHUP; e.data.ptr = r;
        ok = ::epoll_ctl(q_, op == 0 ? EPOLL_CTL_ADD : op == 1 ? EPOLL_CTL_MOD : EPOLL_CTL_DEL, fd, &e) == 0;
#elif defined(FORGE_OS_KQUEUE)
        struct kevent k[2]; int n = 0;
        unsigned want = op == 2 ? 0 : ev, had = op == 0 ? 0 : r->ev;
        if ((want ^ had) & readable) EV_SET(&k[n++], fd, EVFILT_READ, (want & readable) ? EV_ADD : EV_DELETE, 0, 0, r);
        if ((want ^ had) & writable) EV_SET(&k[n++], fd, EVFILT_WRITE, (want & writable) ? EV_ADD : EV_DELETE, 0, 0, r);
        if (op == 1 && !((want ^ had) & (readable | writable)) && want) { /* only user changed */ }
        ok = n == 0 || ::kevent(q_, k, n, nullptr, 0, nullptr) == 0;
#endif
        if (op == 2) { for (std::size_t i = 0; i < regs_.size(); ++i) if (regs_[i] == r) { delete r; regs_.erase(regs_.begin() + std::ptrdiff_t(i)); break; } }
        else { r->ev = ev; r->user = user; }
        if (!ok && op == 0) { delete regs_.back(); regs_.pop_back(); }
        return ok;
    }
#if defined(FORGE_OS_EPOLL) || defined(FORGE_OS_KQUEUE)
    int q_ = -1;
#endif
    std::vector<reg*> regs_;
public:
    poller(poller&&) = delete;
    void clear() { for (auto* r : regs_) delete r; regs_.clear(); }
};

// ---------------------------------------------------------------- completion queue
// Completion-based async I/O: IOCP on Windows. Elsewhere the same API is backed by a worker thread doing
// blocking pread/pwrite, so callers are portable (readiness polling stays in poller above).
// Every read_async/write_async that returns true produces exactly one completion carrying its key: failures the
// OS reports at submission time arrive as a completion with `error` set, like failures during the transfer. A
// read at end of file completes with 0 bytes and no error. false means nothing was submitted (and nothing will
// complete); last_errno() says why. Keys are the caller's: the whole 64-bit range is free, post() included.
// Windows: an os::file is opened for synchronous I/O, so the port reopens it overlapped (ReOpenFile) once and
// caches that handle per file identity (volume + file id). forget(f) closes the cached handle (deferred while
// I/O on it is in flight); entries whose original handle was closed or now names another file are dropped
// when they are next looked at, and on the next new association.
struct completion { std::uint64_t key = 0; std::size_t bytes = 0; int error = 0; void* user = nullptr; };
class completion_port {
public:
    completion_port() {
#if defined(FORGE_OS_WINDOWS)
        h_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
#endif
    }
    ~completion_port() {
#if defined(FORGE_OS_WINDOWS)
        // Cancel what is in flight and wait for it: the kernel writes into the caller's buffers and our OVERLAPPEDs
        // until each operation has completed.
        std::size_t pending = 0;
        for (auto& e : assoc_) { pending += e->pending; if (e->pending) CancelIoEx(e->ov, nullptr); }
        while (h_ && pending) {
            DWORD n = 0; ULONG_PTR k = 0; LPOVERLAPPED ov = nullptr;
            GetQueuedCompletionStatus(h_, &n, &k, &ov, 5000);
            if (!ov) break;
            if (k == post_key) { delete reinterpret_cast<post_t*>(ov); continue; }
            delete reinterpret_cast<op_t*>(ov); --pending;
        }
        while (h_) {   // leftover posts
            DWORD n = 0; ULONG_PTR k = 0; LPOVERLAPPED ov = nullptr;
            GetQueuedCompletionStatus(h_, &n, &k, &ov, 0);
            if (!ov) break;
            if (k == post_key) delete reinterpret_cast<post_t*>(ov); else delete reinterpret_cast<op_t*>(ov);
        }
        for (auto& e : assoc_) CloseHandle(e->ov);
        if (h_) CloseHandle(h_);
#endif
    }
    completion_port(const completion_port&) = delete; completion_port& operator=(const completion_port&) = delete;
    const char* backend() const {
#if defined(FORGE_OS_WINDOWS)
        return "iocp";
#else
        return "emulated";
#endif
    }
    // Wake a waiter with a user completion.
    bool post(std::uint64_t key, void* user = nullptr, std::size_t bytes = 0) { return post_(key, user, bytes, 0); }
    // Asynchronous positional read into buf; completes with `key`. Windows moves at most 1 GiB per operation.
    bool read_async(file& f, void* buf, std::size_t n, std::uint64_t off, std::uint64_t key) {
#if defined(FORGE_OS_WINDOWS)
        return issue_(f, buf, n, off, key, false);
#else
        auto r = f.pread(buf, n, off);
        lock_guard g(m_); q_.push_back({key, r.value, r.err.code, nullptr}); c_.notify_one(); return true;
#endif
    }
    bool write_async(file& f, const void* buf, std::size_t n, std::uint64_t off, std::uint64_t key) {
#if defined(FORGE_OS_WINDOWS)
        return issue_(f, const_cast<void*>(buf), n, off, key, true);
#else
        auto r = f.pwrite(buf, n, off);
        lock_guard g(m_); q_.push_back({key, r.value, r.err.code, nullptr}); c_.notify_one(); return true;
#endif
    }
    // Drop this port's state for f (call before closing f to release the reopened handle at once).
    void forget(const file& f) {
#if defined(FORGE_OS_WINDOWS)
        lock_guard g(m_);
        for (std::size_t i = 0; i < assoc_.size(); ++i) if (assoc_[i]->orig == f.native()) { retire_(i); break; }
#else
        (void)f;
#endif
    }
    // Wait up to timeout_ms (-1 forever) for one completion.
    bool wait(completion& out, int timeout_ms) {
#if defined(FORGE_OS_WINDOWS)
        DWORD n = 0; ULONG_PTR k = 0; LPOVERLAPPED ov = nullptr;
        BOOL ok = GetQueuedCompletionStatus(h_, &n, &k, &ov, timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
        if (!ov) return false;   // timeout (or the port is gone)
        DWORD err = ok ? 0 : GetLastError();
        if (k == post_key) { auto* p = reinterpret_cast<post_t*>(ov); out = {p->key, p->bytes, p->error, p->user}; delete p; return true; }
        auto* op = reinterpret_cast<op_t*>(ov);
        if (err == ERROR_HANDLE_EOF) err = 0;   // read at end of file: 0 bytes, like pread
        out = {op->key, n, int(err), nullptr};
        { lock_guard g(m_); if (op->e) { --op->e->pending; for (std::size_t i = 0; i < assoc_.size(); ++i) if (assoc_[i].get() == op->e) { if (op->e->stale && !op->e->pending) drop_(i); break; } } }
        delete op; return true;
#else
        lock_guard g(m_);
        std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms < 0 ? 0 : timeout_ms) * 1000000ull;
        while (q_.empty()) {
            if (timeout_ms < 0) c_.wait(m_);
            else { auto now = monotonic_ns(); if (now >= end) return false; c_.wait_for(m_, end - now); }
        }
        out = q_.front(); q_.erase(q_.begin()); return true;
#endif
    }
private:
    bool post_(std::uint64_t key, void* user, std::size_t bytes, int err) {
#if defined(FORGE_OS_WINDOWS)
        auto* p = new post_t{key, user, bytes, err};
        if (PostQueuedCompletionStatus(h_, 0, post_key, reinterpret_cast<LPOVERLAPPED>(p))) return true;
        DWORD e = GetLastError(); delete p; SetLastError(e); return false;
#else
        lock_guard g(m_); q_.push_back({key, bytes, err, user}); c_.notify_one(); return true;
#endif
    }
#if defined(FORGE_OS_WINDOWS)
    // All completion keys on the port are ours: user posts travel in a post_t, I/O in an op_t.
    static constexpr ULONG_PTR io_key = 1, post_key = 2;
    struct entry { HANDLE orig = nullptr, ov = nullptr; DWORD vol = 0; std::uint64_t id = 0; std::size_t pending = 0; bool stale = false; };
    struct op_t { OVERLAPPED ov{}; std::uint64_t key = 0; entry* e = nullptr; };
    struct post_t { std::uint64_t key; void* user; std::size_t bytes; int error; };
    static bool file_id_(HANDLE h, DWORD& vol, std::uint64_t& id) {
        BY_HANDLE_FILE_INFORMATION i{}; if (!GetFileInformationByHandle(h, &i)) return false;
        vol = i.dwVolumeSerialNumber; id = (std::uint64_t(i.nFileIndexHigh) << 32) | i.nFileIndexLow; return true;
    }
    void drop_(std::size_t i) { CloseHandle(assoc_[i]->ov); assoc_.erase(assoc_.begin() + std::ptrdiff_t(i)); }
    void retire_(std::size_t i) { if (assoc_[i]->pending) assoc_[i]->stale = true; else drop_(i); }
    bool issue_(file& f, void* buf, std::size_t n, std::uint64_t off, std::uint64_t key, bool wr) {
        auto* op = new op_t{}; op->ov.Offset = DWORD(off); op->ov.OffsetHigh = DWORD(off >> 32); op->key = key;
        {
            lock_guard g(m_);
            entry* e = associate_(f.native());
            if (!e) { DWORD x = GetLastError(); delete op; SetLastError(x); return false; }
            op->e = e; ++e->pending;
        }
        DWORD len = file::chunk(n);
        BOOL ok = wr ? WriteFile(op->e->ov, buf, len, nullptr, &op->ov) : ReadFile(op->e->ov, buf, len, nullptr, &op->ov);
        DWORD err = ok ? 0 : GetLastError();
        if (ok || err == ERROR_IO_PENDING) return true;   // the completion packet is (or will be) queued
        // Failed at submission: no packet is queued, so deliver the outcome as this key's completion.
        { lock_guard g(m_); --op->e->pending; }
        delete op;
        return post_(key, nullptr, 0, err == ERROR_HANDLE_EOF ? 0 : int(err));
    }
    // file handles are opened synchronously; IOCP needs FILE_FLAG_OVERLAPPED, so reopen the same file overlapped once.
    // Caller holds m_.
    entry* associate_(HANDLE f) {
        DWORD vol = 0; std::uint64_t id = 0; if (!file_id_(f, vol, id)) return nullptr;
        for (std::size_t i = 0; i < assoc_.size(); ++i) {
            entry* e = assoc_[i].get(); if (e->orig != f || e->stale) continue;
            if (e->vol == vol && e->id == id) return e;
            retire_(i); break;   // the handle value was closed and reused for another file
        }
        for (std::size_t i = assoc_.size(); i-- > 0;) {   // prune entries whose original handle is gone
            entry* e = assoc_[i].get(); DWORD v2 = 0; std::uint64_t id2 = 0;
            if (!e->stale && (!file_id_(e->orig, v2, id2) || v2 != e->vol || id2 != e->id)) retire_(i);
        }
        // Reopen with exactly the access the caller's handle has (ReOpenFile checks the file's ACL, not the original
        // handle, so asking for more would let a write-only file be read). NtQueryObject is ntdll's, looked up at run time.
        struct basic_info { ULONG attributes; ACCESS_MASK granted; ULONG handles, pointers, reserved[10]; };
        using query_fn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
        static const auto query = reinterpret_cast<query_fn>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryObject")));
        basic_info bi{}; ULONG got = 0;
        bool known = query && query(f, 0 /*ObjectBasicInformation*/, &bi, sizeof bi, &got) >= 0 && (bi.granted & (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA));
        HANDLE o = INVALID_HANDLE_VALUE;
        if (known) o = ReOpenFile(f, bi.granted & (FILE_ALL_ACCESS & ~DWORD(SYNCHRONIZE)), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
        else for (DWORD acc : {DWORD(GENERIC_READ | GENERIC_WRITE), DWORD(GENERIC_READ), DWORD(GENERIC_WRITE)}) {
            o = ReOpenFile(f, acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_FLAG_OVERLAPPED);
            if (o != INVALID_HANDLE_VALUE) break;
        }
        if (o == INVALID_HANDLE_VALUE) return nullptr;
        if (!CreateIoCompletionPort(o, h_, io_key, 0)) { DWORD x = GetLastError(); CloseHandle(o); SetLastError(x); return nullptr; }
        auto e = std::make_unique<entry>(); e->orig = f; e->ov = o; e->vol = vol; e->id = id;
        assoc_.push_back(std::move(e)); return assoc_.back().get();
    }
    HANDLE h_ = nullptr; mutex m_; std::vector<std::unique_ptr<entry>> assoc_;
#else
    mutex m_; condvar c_; std::vector<completion> q_;
#endif
};

} // namespace forge::os
