#pragma once
// Local IPC streams: Unix-domain sockets (Linux/Android/macOS/iOS/FreeBSD; Linux & Android use the abstract
// namespace when the name starts with '@') and Windows named pipes (\\.\pipe\<name>).
// Names: a filesystem path (or "@abstract") on POSIX; a bare pipe name on Windows.  local_endpoint(name) builds
// a sensible default for both.
// Security: abstract Linux sockets and Windows pipes (default DACL: read for Everyone) can be reached by other
// users of the machine, so a server that cares checks peer() (uid / SID and pid of the other end). Clients
// connect with SECURITY_IDENTIFICATION, so a hostile pipe server cannot impersonate them. listen() never
// deletes anything but a stale socket at the path.
#include <forge/os/detail.hpp>
#if defined(FORGE_OS_POSIX)
#include <sys/un.h>
#endif
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
#include <sys/ucred.h>
#endif
#if defined(FORGE_OS_WINDOWS)
#include <sddl.h>
#endif

namespace forge::os {
inline std::string local_endpoint(const std::string& name) {
#if defined(FORGE_OS_WINDOWS)
    return name;
#elif defined(__linux__)
    // Abstract namespace on Linux/Android: no socket file, so it also works where SELinux forbids creating one
    // (Android shell/app data dirs). Pass "@name" explicitly, or a bare name to get "@forge-<name>".
    return name.size() && (name[0] == '@' || name[0] == '/') ? name : "@forge-" + name;
#else
    return name.size() && name[0] == '/' ? name : join(temp_dir(), name + ".sock");
#endif
}
// The process at the other end of a local_stream. uid/gid on POSIX (gid -1 where the OS does not say), the user
// SID string on Windows; pid where the OS reports it (-1 otherwise).
struct peer_credentials { std::int64_t pid = -1, uid = -1, gid = -1; std::string sid; };
class local_stream {
public:
#if defined(FORGE_OS_WINDOWS)
    using native_t = HANDLE; static native_t bad() { return INVALID_HANDLE_VALUE; }
#else
    using native_t = int; static native_t bad() { return -1; }
#endif
    local_stream() = default;
    explicit local_stream(native_t h) : h_(h) {}
    local_stream(local_stream&& o) noexcept : h_(std::exchange(o.h_, bad())) {}
    local_stream& operator=(local_stream&& o) noexcept { if (this != &o) { close(); h_ = std::exchange(o.h_, bad()); } return *this; }
    ~local_stream() { close(); }
    bool valid() const { return h_ != bad(); }
    native_t native() const { return h_; }
    static result<local_stream> connect(const std::string& name, int timeout_ms = 2000) {
#if defined(FORGE_OS_WINDOWS)
        auto w = widen("\\\\.\\pipe\\" + name); std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms) * 1000000ull;
        for (;;) {
            // SECURITY_IDENTIFICATION: the server may learn who we are but cannot act as us.
            HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (h != INVALID_HANDLE_VALUE) return {local_stream(h), {}};
            DWORD e = GetLastError();
            if ((e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) || monotonic_ns() > end) { SetLastError(e); return {{}, last_error("connect pipe " + name)}; }
            if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(w.c_str(), 100); else Sleep(10);
        }
#else
        sockaddr_un a{}; socklen_t len = 0; if (auto e = addr_(name, a, len)) return {{}, e};
        int fd = cloexec_socket(AF_UNIX, SOCK_STREAM); if (fd < 0) return {{}, last_error("socket(AF_UNIX)")};
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms) * 1000000ull;
        while (::connect(fd, reinterpret_cast<sockaddr*>(&a), len)) {
            if (errno == EINTR) continue;
            if ((errno != ENOENT && errno != ECONNREFUSED && errno != EAGAIN) || monotonic_ns() > end) { auto e = last_error("connect " + name); ::close(fd); return {{}, e}; }
            sleep_ns(5000000);
        }
        return {local_stream(fd), {}};
#endif
    }
    result<std::size_t> read(void* p, std::size_t n) {
#if defined(FORGE_OS_WINDOWS)
        DWORD got = 0; if (!io_(false, p, n, got)) { if (GetLastError() == ERROR_BROKEN_PIPE) return {0, {}}; return {0, last_error("pipe read")}; }
        return {got, {}};
#else
        for (;;) { auto r = ::recv(h_, p, n, 0); if (r >= 0) return {std::size_t(r), {}}; if (errno != EINTR) return {0, last_error("recv")}; }
#endif
    }
    status write_all(const void* p, std::size_t n) {
        auto* b = static_cast<const char*>(p);
        while (n) {
#if defined(FORGE_OS_WINDOWS)
            DWORD w = 0; if (!io_(true, const_cast<char*>(b), n, w)) return {{}, last_error("pipe write")};
#else
#if defined(MSG_NOSIGNAL)
            auto w = ::send(h_, b, n, MSG_NOSIGNAL);
#else
            auto w = ::send(h_, b, n, 0);
#endif
            if (w < 0) { if (errno == EINTR) continue; return {{}, last_error("send")}; }
#endif
            b += w; n -= std::size_t(w);
        }
        return {};
    }
    status read_exact(void* p, std::size_t n) {
        auto* b = static_cast<char*>(p);
        while (n) { auto r = read(b, n); if (!r) return {{}, r.err}; if (!r.value) return {{}, {-1, "local_stream: eof"}}; b += r.value; n -= r.value; }
        return {};
    }
    // Who is at the other end (either side may ask).
    result<peer_credentials> peer() const {
        peer_credentials c;
#if defined(FORGE_OS_WINDOWS)
        ULONG pid = 0;
        if (!GetNamedPipeClientProcessId(h_, &pid)) return {{}, last_error("GetNamedPipeClientProcessId")};
        if (pid == GetCurrentProcessId()) { ULONG s = 0; if (GetNamedPipeServerProcessId(h_, &s)) pid = s; }   // we are the client
        c.pid = pid;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            HANDLE t = nullptr;
            if (OpenProcessToken(p, TOKEN_QUERY, &t)) {
                DWORD n = 0; GetTokenInformation(t, TokenUser, nullptr, 0, &n);
                std::vector<std::uint8_t> b(n ? n : 1);
                wchar_t* s = nullptr;
                if (n && GetTokenInformation(t, TokenUser, b.data(), n, &n) && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid, &s)) { c.sid = narrow(s); LocalFree(s); }
                CloseHandle(t);
            }
            CloseHandle(p);
        }
        return {std::move(c), {}};
#elif defined(__linux__)
        ucred u{}; socklen_t l = sizeof u;
        if (::getsockopt(h_, SOL_SOCKET, SO_PEERCRED, &u, &l)) return {{}, last_error("SO_PEERCRED")};
        c.pid = u.pid; c.uid = u.uid; c.gid = u.gid; return {std::move(c), {}};
#else
        uid_t u = 0; gid_t g = 0;
        if (::getpeereid(h_, &u, &g)) return {{}, last_error("getpeereid")};
        c.uid = u; c.gid = g;
#if defined(__APPLE__) && defined(LOCAL_PEERPID)
        pid_t pid = 0; socklen_t l = sizeof pid; if (!::getsockopt(h_, SOL_LOCAL, LOCAL_PEERPID, &pid, &l)) c.pid = pid;
#elif defined(FORGE_OS_FREEBSD) && defined(LOCAL_PEERCRED)
        xucred x{}; socklen_t l = sizeof x; if (!::getsockopt(h_, SOL_LOCAL, LOCAL_PEERCRED, &x, &l) && x.cr_version == XUCRED_VERSION) c.pid = x.cr_pid;
#endif
        return {std::move(c), {}};
#endif
    }
    void close() {
        if (!valid()) return;
#if defined(FORGE_OS_WINDOWS)
        FlushFileBuffers(h_); CloseHandle(h_);
#else
        ::close(h_);
#endif
        h_ = bad();
    }
#if defined(FORGE_OS_POSIX)
    static error addr_(const std::string& name, sockaddr_un& a, socklen_t& len) {
        a.sun_family = AF_UNIX;
        if (name.empty()) return errno_error(EINVAL, "unix socket: empty name");
        if (name.size() >= sizeof a.sun_path) return errno_error(ENAMETOOLONG, "unix socket path too long: " + name);
        std::memcpy(a.sun_path, name.data(), name.size());
        len = socklen_t(offsetof(sockaddr_un, sun_path) + name.size() + (name[0] == '@' ? 0 : 1));
#if defined(__linux__)
        if (name[0] == '@') a.sun_path[0] = '\0';
#else
        if (name[0] == '@') return unsupported("abstract unix sockets");
#endif
        return {};
    }
#endif
private:
#if defined(FORGE_OS_WINDOWS)
    // Overlapped-safe blocking I/O: server ends are created FILE_FLAG_OVERLAPPED, clients are not; this works for both.
    bool io_(bool wr, void* p, std::size_t n, DWORD& got) {
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); if (!ov.hEvent) return false;
        DWORD len = file::chunk(n);
        BOOL ok = wr ? WriteFile(h_, p, len, nullptr, &ov) : ReadFile(h_, p, len, nullptr, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) { DWORD e = GetLastError(); CloseHandle(ov.hEvent); SetLastError(e); return false; }
        ok = GetOverlappedResult(h_, &ov, &got, TRUE); DWORD e = GetLastError(); CloseHandle(ov.hEvent); SetLastError(e); return ok != 0;
    }
#endif
    native_t h_ = bad();
};

class local_listener {
public:
    local_listener() = default;
    local_listener(local_listener&& o) noexcept { *this = std::move(o); }
    local_listener& operator=(local_listener&& o) noexcept {
        if (this != &o) { close(); name_ = std::move(o.name_);
#if defined(FORGE_OS_WINDOWS)
            next_ = std::exchange(o.next_, INVALID_HANDLE_VALUE);
#else
            fd_ = std::exchange(o.fd_, -1); dev_ = o.dev_; ino_ = o.ino_;
#endif
        }
        return *this;
    }
    ~local_listener() { close(); }
    static result<local_listener> listen(const std::string& name) {
        local_listener l; l.name_ = name;
#if defined(FORGE_OS_WINDOWS)
        l.next_ = l.make_pipe_(true);   // FIRST_PIPE_INSTANCE: fails if another process already owns the name
        if (l.next_ == INVALID_HANDLE_VALUE) return {std::move(l), last_error("CreateNamedPipe " + name)};
        return {std::move(l), {}};
#else
        sockaddr_un a{}; socklen_t len = 0; if (auto e = local_stream::addr_(name, a, len)) return {std::move(l), e};
        if (name[0] != '@') {   // a stale socket from a dead server is replaced; any other file is left alone (bind then fails)
            struct stat st{}; if (::lstat(name.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) ::unlink(name.c_str());
        }
        l.fd_ = cloexec_socket(AF_UNIX, SOCK_STREAM); if (l.fd_ < 0) return {std::move(l), last_error("socket(AF_UNIX)")};
        if (::bind(l.fd_, reinterpret_cast<sockaddr*>(&a), len)) { auto e = last_error("bind " + name); ::close(l.fd_); l.fd_ = -1; return {std::move(l), e}; }
        if (name[0] != '@') { struct stat st{}; if (::lstat(name.c_str(), &st) == 0) { l.dev_ = st.st_dev; l.ino_ = st.st_ino; } }
        if (::listen(l.fd_, 64)) return {std::move(l), last_error("listen " + name)};
        return {std::move(l), {}};
#endif
    }
    // Block up to timeout_ms (-1 forever) for one client.
    result<local_stream> accept(int timeout_ms = -1) {
#if defined(FORGE_OS_WINDOWS)
        if (next_ == INVALID_HANDLE_VALUE && (next_ = make_pipe_(false)) == INVALID_HANDLE_VALUE) return {{}, last_error("CreateNamedPipe " + name_)};
        for (;;) {
            OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) return {{}, last_error("CreateEvent")};
            BOOL ok = ConnectNamedPipe(next_, &ov); DWORD e = ok ? 0 : GetLastError();
            if (!ok && e == ERROR_IO_PENDING) {
                DWORD w = WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
                DWORD we = w == WAIT_FAILED ? GetLastError() : 0;
                // Timed out (or the wait failed): cancel, then wait for the cancellation to land, because the kernel
                // completes into `ov` and its event, which live on this stack frame. A client may still have
                // connected in the meantime; then hand it out.
                if (w != WAIT_OBJECT_0) CancelIoEx(next_, &ov);
                DWORD x; ok = GetOverlappedResult(next_, &ov, &x, TRUE); e = ok ? 0 : GetLastError();
                if (!ok && e == ERROR_OPERATION_ABORTED) {
                    CloseHandle(ov.hEvent);
                    if (we) return {{}, {int(we), "accept: wait failed: " + error_text(int(we))}};
                    return {{}, {WAIT_TIMEOUT, "accept: timeout", err_domain::win32}};
                }
            }
            CloseHandle(ov.hEvent);
            if (!ok && e == ERROR_NO_DATA) { DisconnectNamedPipe(next_); continue; }   // the client already left
            if (!ok && e != ERROR_PIPE_CONNECTED) return {{}, {int(e), "ConnectNamedPipe: " + error_text(int(e))}};
            break;
        }
        // Hand out the connected instance (opened overlapped; local_stream's I/O works on both kinds) and open the
        // next listening instance. If that fails, the next accept() retries it.
        HANDLE c = std::exchange(next_, make_pipe_(false));
        return {local_stream(c), {}};
#else
        if (timeout_ms >= 0) {
            pollfd p{fd_, POLLIN, 0}; std::uint64_t end = monotonic_ns() + std::uint64_t(timeout_ms) * 1000000ull;
            for (;;) {
                auto now = monotonic_ns(); int left = now >= end ? 0 : int((end - now + 999999) / 1000000);
                int r = ::poll(&p, 1, left);
                if (r > 0) break;
                if (r == 0) return {{}, errno_error(ETIMEDOUT, "accept: timeout")};
                if (errno != EINTR) return {{}, last_error("poll")};
            }
        }
        int c = cloexec_accept(fd_); if (c < 0) return {{}, last_error("accept")};
#if defined(__APPLE__) || defined(FORGE_OS_FREEBSD)
        int one = 1; ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        return {local_stream(c), {}};
#endif
    }
    const std::string& name() const { return name_; }
    void close() {
#if defined(FORGE_OS_WINDOWS)
        if (next_ != INVALID_HANDLE_VALUE) { CloseHandle(next_); next_ = INVALID_HANDLE_VALUE; }
#else
        if (fd_ >= 0) {
            ::close(fd_); fd_ = -1;
            if (!name_.empty() && name_[0] != '@') {   // only our own socket: someone may have replaced the path since
                struct stat st{}; if (::lstat(name_.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == dev_ && st.st_ino == ino_) ::unlink(name_.c_str());
            }
        }
#endif
    }
private:
#if defined(FORGE_OS_WINDOWS)
    HANDLE make_pipe_(bool first) {
        return CreateNamedPipeW(widen("\\\\.\\pipe\\" + name_).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    }
    HANDLE next_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1; dev_t dev_ = 0; ino_t ino_ = 0;
#endif
    std::string name_;
};
} // namespace forge::os
