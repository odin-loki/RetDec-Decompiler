#pragma once
// forge::os anonymous pipes with RAII ends and deadline reads. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>

namespace forge::os {

#if defined(FORGE_OS_POSIX)
namespace detail {
// pipe() with both ends close-on-exec from the start, so a fork+exec on another thread never inherits them (a
// leaked write end keeps the reader from ever seeing EOF). pipe2(O_CLOEXEC) is atomic on Linux, Android and
// FreeBSD. Apple has no pipe2: pipe + FD_CLOEXEC run under fork_mutex(), which every FORGE fork
// (serialized_fork) takes, so no FORGE-spawned child can inherit the ends in between; a fork the embedding
// program makes without serialized_fork can still race there. A child that needs an end as 0/1/2 gets it with
// dup2, which clears the flag on the copy (see detail_proc::dup_to for the from == to case).
inline int pipe_cloexec(int fd[2]) noexcept {
#if defined(__APPLE__)
    std::lock_guard<std::mutex> lk(fork_mutex());
    if (::pipe(fd)) return -1;
    ::fcntl(fd[0], F_SETFD, FD_CLOEXEC); ::fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    return 0;
#else
    return ::pipe2(fd, O_CLOEXEC);
#endif
}
}
class pipe_end {
public:
    pipe_end() = default;
    explicit pipe_end(int fd) : fd_(fd) {}
    pipe_end(const pipe_end&) = delete; pipe_end& operator=(const pipe_end&) = delete;
    pipe_end(pipe_end&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    pipe_end& operator=(pipe_end&& o) noexcept { if (this != &o) { close(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    ~pipe_end() { close(); }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    int native() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    status write_all(std::string_view s) {
        std::size_t off = 0;
        while (off < s.size()) { auto n = ::write(fd_, s.data() + off, s.size() - off); if (n < 0) { if (errno == EINTR) continue; return {{}, last_error("write")}; } off += std::size_t(n); }
        return {};
    }
    // Read what is available within timeout_ms (0 = block until data or EOF). Empty = EOF or timeout.
    std::string read_some(std::size_t max = 65536, int timeout_ms = 0) {
        if (fd_ < 0) return {};
        if (timeout_ms > 0) {
            struct pollfd f{fd_, POLLIN, 0}; std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms) * 1000000ull;
            for (;;) {   // a signal (EINTR) is neither data nor EOF: wait out the rest of the timeout
                auto now = monotonic_ns(); int r = ::poll(&f, 1, now >= end ? 0 : int((end - now + 999999) / 1000000));
                if (r > 0) break; if (r == 0 || errno != EINTR) return {};
            }
        }
        std::string s(max, '\0'); ssize_t n;
        do n = ::read(fd_, s.data(), max); while (n < 0 && errno == EINTR);
        if (n <= 0) return {};
        s.resize(std::size_t(n)); return s;
    }
    std::string read_all() { std::string o, c; while (!(c = read_some()).empty()) o += c; return o; }
private:
    int fd_ = -1;
};
struct pipe_pair { pipe_end read, write; };
inline result<pipe_pair> make_pipe() {
    int f[2]; if (detail::pipe_cloexec(f)) return {{}, last_error("pipe")};
    pipe_pair p{pipe_end(f[0]), pipe_end(f[1])};
    return {std::move(p), {}};
}
#elif defined(FORGE_OS_WINDOWS)
// Windows: the read end is an overlapped named-pipe server (so read_some can time out); the write end a plain
// synchronous client. Both are non-inheritable; hand an end to a child through process.hpp's std redirection.
class pipe_end {
public:
    pipe_end() = default;
    explicit pipe_end(HANDLE h) : h_(h) {}
    pipe_end(const pipe_end&) = delete; pipe_end& operator=(const pipe_end&) = delete;
    pipe_end(pipe_end&& o) noexcept : h_(std::exchange(o.h_, INVALID_HANDLE_VALUE)) {}
    pipe_end& operator=(pipe_end&& o) noexcept { if (this != &o) { close(); h_ = std::exchange(o.h_, INVALID_HANDLE_VALUE); } return *this; }
    ~pipe_end() { close(); }
    void close() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; } }
    HANDLE native() const { return h_; }
    explicit operator bool() const { return h_ != INVALID_HANDLE_VALUE; }
    status write_all(std::string_view s) {
        std::size_t off = 0;
        while (off < s.size()) {
            DWORD n = 0; if (!io_(true, const_cast<char*>(s.data() + off), s.size() - off, n, -1)) return {{}, last_error("write")};
            off += n;
        }
        return {};
    }
    // Read what is available within timeout_ms (0 = block until data or EOF). Empty = EOF or timeout.
    std::string read_some(std::size_t max = 65536, int timeout_ms = 0) {
        if (h_ == INVALID_HANDLE_VALUE || !max) return {};
        std::string s(max, '\0'); DWORD n = 0;
        if (!io_(false, s.data(), max, n, timeout_ms > 0 ? timeout_ms : -1)) return {};
        s.resize(n); return s;
    }
    std::string read_all() { std::string o, c; while (!(c = read_some()).empty()) o += c; return o; }
private:
    // Overlapped I/O with an optional timeout; works on synchronous handles too (they just complete inline). On
    // timeout the read is cancelled and waited for, since the kernel completes into this stack frame; data that
    // arrived meanwhile is still returned.
    bool io_(bool wr, void* p, std::size_t len, DWORD& got, int timeout_ms) {
        got = 0;
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!ov.hEvent) return false;
        DWORD n = DWORD(len < (std::size_t(1) << 30) ? len : (std::size_t(1) << 30));
        BOOL ok = wr ? WriteFile(h_, p, n, nullptr, &ov) : ReadFile(h_, p, n, nullptr, &ov);
        DWORD e = ok ? 0 : GetLastError();
        if (!ok && e == ERROR_IO_PENDING) {
            if (timeout_ms >= 0 && WaitForSingleObject(ov.hEvent, DWORD(timeout_ms)) != WAIT_OBJECT_0) CancelIoEx(h_, &ov);
            ok = GetOverlappedResult(h_, &ov, &got, TRUE); e = ok ? 0 : GetLastError();
        } else if (ok) GetOverlappedResult(h_, &ov, &got, TRUE);
        CloseHandle(ov.hEvent); SetLastError(e);
        return ok != 0;   // ERROR_BROKEN_PIPE (EOF), ERROR_OPERATION_ABORTED (timeout) and real errors all read as false
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
};
struct pipe_pair { pipe_end read, write; };
inline result<pipe_pair> make_pipe() {
    static std::atomic<unsigned> serial{0};
    std::uint64_t r = 0; entropy(&r, sizeof r);
    char name[96]; std::snprintf(name, sizeof name, "\\\\.\\pipe\\forge-anon-%lu-%u-%016llx", static_cast<unsigned long>(GetCurrentProcessId()),
                                 serial.fetch_add(1), static_cast<unsigned long long>(r));
    auto w = widen(name);
    HANDLE rd = CreateNamedPipeW(w.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
    if (rd == INVALID_HANDLE_VALUE) return {{}, last_error("CreateNamedPipe")};
    HANDLE wr = CreateFileW(w.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (wr == INVALID_HANDLE_VALUE) { auto e = last_error("open pipe"); CloseHandle(rd); return {{}, e}; }
    pipe_pair p{pipe_end(rd), pipe_end(wr)};
    return {std::move(p), {}};
}
#endif

} // namespace forge::os
