#pragma once
// forge::os::fs: file-system operations beyond os.hpp's file/stat/list_dir: copy (file / tree), links,
// permissions and ownership, timestamps, glob, recursive walk, advisory file locks, disk usage, mounts,
// temp files/dirs and atomic writes. std::filesystem underneath where it is portable; native calls for the
// rest. Nothing throws: every call returns os::result / os::status. Guide: docs/os-wrapper.md.
#include <forge/os/os.hpp>
#include <forge/os/private_files.hpp>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#if defined(FORGE_OS_POSIX)
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#endif
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif

namespace forge::os::fs {
namespace stdfs = std::filesystem;

// std::filesystem reports errno values (generic_category: libstdc++, including mingw) or native codes
// (system_category: Win32 on MSVC); the error records which, so os::classify reads it right.
inline error from_ec(const std::error_code& ec, std::string_view what) {
    return {ec.value() ? ec.value() : -1, std::string(what) + ": " + ec.message(), ec.category() == std::generic_category() ? err_domain::posix : err_domain::native};
}
inline stdfs::path u8path(const std::string& s) { return stdfs::path(std::u8string(s.begin(), s.end())); }
inline std::string u8str(const stdfs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }

// ---------------------------------------------------------------- copy / move / remove
struct copy_opts { bool overwrite = false, recursive = true, follow_symlinks = false, skip_existing = false; };
inline status copy(const std::string& from, const std::string& to, copy_opts o = {}) {
    std::error_code ec;
    auto opt = stdfs::copy_options::none;
    if (o.overwrite) opt |= stdfs::copy_options::overwrite_existing;
    else if (o.skip_existing) opt |= stdfs::copy_options::skip_existing;
    if (o.recursive) opt |= stdfs::copy_options::recursive;
    opt |= o.follow_symlinks ? stdfs::copy_options::none : stdfs::copy_options::copy_symlinks;
    stdfs::copy(u8path(from), u8path(to), opt, ec);
    if (ec) return {{}, from_ec(ec, "copy " + from + " -> " + to)};
    return {};
}
inline status copy_tree(const std::string& from, const std::string& to, bool overwrite = false) { return copy(from, to, {overwrite, true, false, !overwrite}); }
inline status move(const std::string& from, const std::string& to) {
    std::error_code ec; stdfs::rename(u8path(from), u8path(to), ec);
    if (ec == std::errc::cross_device_link) {   // different file systems: copy then remove
        if (auto s = copy(from, to, {true, true, false, false}); !s) return s;
        stdfs::remove_all(u8path(from), ec);
    }
    if (ec) return {{}, from_ec(ec, "move " + from + " -> " + to)};
    return {};
}
inline result<std::uint64_t> remove_all(const std::string& p) {
    std::error_code ec; auto n = stdfs::remove_all(u8path(p), ec);
    if (ec) return {0, from_ec(ec, "remove " + p)};
    return {std::uint64_t(n), {}};
}
inline status make_dirs(const std::string& p) { std::error_code ec; stdfs::create_directories(u8path(p), ec); if (ec) return {{}, from_ec(ec, "mkdir " + p)}; return {}; }
inline bool exists(const std::string& p) { std::error_code ec; return stdfs::exists(u8path(p), ec); }
inline bool is_dir(const std::string& p) { std::error_code ec; return stdfs::is_directory(u8path(p), ec); }
inline bool is_file(const std::string& p) { std::error_code ec; return stdfs::is_regular_file(u8path(p), ec); }
inline bool is_symlink(const std::string& p) { std::error_code ec; return stdfs::is_symlink(u8path(p), ec); }
inline result<std::uint64_t> file_size(const std::string& p) { std::error_code ec; auto n = stdfs::file_size(u8path(p), ec); if (ec) return {0, from_ec(ec, "size " + p)}; return {std::uint64_t(n), {}}; }
inline result<std::string> absolute(const std::string& p) { std::error_code ec; auto a = stdfs::weakly_canonical(stdfs::absolute(u8path(p), ec), ec); if (ec) return {{}, from_ec(ec, "absolute " + p)}; return {u8str(a), {}}; }
inline std::string current_dir() { std::error_code ec; return u8str(stdfs::current_path(ec)); }
inline status set_current_dir(const std::string& p) { std::error_code ec; stdfs::current_path(u8path(p), ec); if (ec) return {{}, from_ec(ec, "cd " + p)}; return {}; }

// ---------------------------------------------------------------- links
inline status symlink(const std::string& target, const std::string& link) {
    std::error_code ec;
    if (is_dir(target)) stdfs::create_directory_symlink(u8path(target), u8path(link), ec);
    else stdfs::create_symlink(u8path(target), u8path(link), ec);
    if (ec) return {{}, from_ec(ec, "symlink " + link)};
    return {};
}
inline status hardlink(const std::string& target, const std::string& link) { std::error_code ec; stdfs::create_hard_link(u8path(target), u8path(link), ec); if (ec) return {{}, from_ec(ec, "hardlink " + link)}; return {}; }
inline result<std::string> read_link(const std::string& p) { std::error_code ec; auto t = stdfs::read_symlink(u8path(p), ec); if (ec) return {{}, from_ec(ec, "readlink " + p)}; return {u8str(t), {}}; }

// ---------------------------------------------------------------- permissions, ownership, times
inline result<unsigned> mode(const std::string& p) { std::error_code ec; auto s = stdfs::status(u8path(p), ec); if (ec) return {0, from_ec(ec, "stat " + p)}; return {unsigned(s.permissions()) & 07777u, {}}; }
inline status chmod(const std::string& p, unsigned mode) {
    std::error_code ec; stdfs::permissions(u8path(p), stdfs::perms(mode & 07777u), stdfs::perm_options::replace, ec);
    if (ec) return {{}, from_ec(ec, "chmod " + p)};
    return {};
}
inline status chown(const std::string& p, long uid, long gid) {
#if defined(FORGE_OS_POSIX)
    if (::lchown(p.c_str(), uid_t(uid), gid_t(gid))) return {{}, last_error("chown " + p)};
    return {};
#else   // NTFS owners are SIDs set through ACL APIs (and need SeRestorePrivilege for anyone but yourself); a
        // numeric uid/gid pair has no faithful mapping, so this stays unsupported.
    (void)p; (void)uid; (void)gid; return {{}, unsupported("chown")};
#endif
}
// Unix epoch minus file_clock epoch, in file_clock ticks (the epoch is implementation-defined; clock_cast is not
// available everywhere). Kept in the clock's own tick type: MSVC's 1601 FILETIME epoch is ~1.3e19 ns ago, which
// overflows int64 nanoseconds. Sampled once; good to well under a millisecond.
inline stdfs::file_time_type::duration file_clock_offset() {
    using fdur = stdfs::file_time_type::duration;
    static const fdur off = [] {
        auto f = stdfs::file_time_type::clock::now(); auto s = std::chrono::system_clock::now();
        fdur v = std::chrono::duration_cast<fdur>(s.time_since_epoch()) - f.time_since_epoch();
        // Every implementation's offset is a whole number of seconds (0, the 1601 FILETIME epoch, libstdc++'s
        // 2174 epoch), and some file clocks tick coarsely (mingw: ~15 ms), so round to the second.
        auto r = std::chrono::round<std::chrono::seconds>(v);
        return std::chrono::duration_cast<fdur>(r);
    }();
    return off;
}
#if defined(FORGE_OS_WINDOWS)
// Windows: FILETIME directly (100 ns ticks since 1601). mingw's libstdc++ last_write_time goes through
// _wstat64/_wutime64 and keeps whole seconds only; MSVC's is fine but this keeps both identical.
namespace detail {
inline constexpr std::int64_t filetime_unix_epoch = 116444736000000000LL;   // 1970-01-01 in FILETIME ticks
inline HANDLE open_for_times(const std::string& p, DWORD access) {
    return CreateFileW(widen(p).c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);   // BACKUP_SEMANTICS: directories too
}
}
inline result<std::int64_t> mtime_ns(const std::string& p) {
    HANDLE h = detail::open_for_times(p, FILE_READ_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return {0, last_error("mtime " + p)};
    FILETIME w{}; BOOL ok = GetFileTime(h, nullptr, nullptr, &w);
    auto err = ok ? error{} : last_error("mtime " + p); CloseHandle(h);
    if (!ok) return {0, err};
    std::int64_t ticks = std::int64_t((std::uint64_t(w.dwHighDateTime) << 32) | w.dwLowDateTime);
    return {(ticks - detail::filetime_unix_epoch) * 100, {}};
}
inline status set_mtime_ns(const std::string& p, std::int64_t ns) {
    std::int64_t ticks = (ns >= 0 ? ns / 100 : -((-ns + 99) / 100)) + detail::filetime_unix_epoch;
    if (ticks < 0) return {{}, error{int(ERROR_INVALID_PARAMETER), "set mtime " + p + ": before 1601"}};
    HANDLE h = detail::open_for_times(p, FILE_WRITE_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return {{}, last_error("set mtime " + p)};
    FILETIME w{DWORD(std::uint64_t(ticks) & 0xffffffffu), DWORD(std::uint64_t(ticks) >> 32)};
    BOOL ok = SetFileTime(h, nullptr, nullptr, &w);
    auto err = ok ? error{} : last_error("set mtime " + p); CloseHandle(h);
    if (!ok) return {{}, err};
    return {};
}
#else
// Modification time in ns since the Unix epoch.
inline result<std::int64_t> mtime_ns(const std::string& p) {
    std::error_code ec; auto t = stdfs::last_write_time(u8path(p), ec);
    if (ec) return {0, from_ec(ec, "mtime " + p)};
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch() + file_clock_offset()).count(), {}};
}
inline status set_mtime_ns(const std::string& p, std::int64_t ns) {
    std::error_code ec;
    auto d = std::chrono::duration_cast<stdfs::file_time_type::duration>(std::chrono::nanoseconds(ns)) - file_clock_offset();
    stdfs::last_write_time(u8path(p), stdfs::file_time_type(d), ec);
    if (ec) return {{}, from_ec(ec, "set mtime " + p)};
    return {};
}
#endif
inline status touch(const std::string& p) {
    if (!exists(p)) { std::ofstream f(u8path(p), std::ios::app); if (!f) return {{}, last_error("touch " + p)}; return {}; }
    return set_mtime_ns(p, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- glob and walk
// Glob pattern match on '/'-separated paths: * and ? never cross '/', ** matches any number of whole
// segments, [abc] / [a-z] / [!x] classes, \ escapes. Case-sensitive.
inline bool glob_match(std::string_view pat, std::string_view s) {
    auto seg_match = [](std::string_view p, std::string_view t) {
        std::size_t pi = 0, ti = 0, star = std::string_view::npos, mark = 0;
        auto cls = [&](std::size_t& i, char c) {   // p[i] == '['
            std::size_t j = i + 1; bool neg = false, hit = false;
            if (j < p.size() && (p[j] == '!' || p[j] == '^')) { neg = true; ++j; }
            std::size_t start = j;
            for (; j < p.size() && (p[j] != ']' || j == start); ++j) {
                if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') { if (c >= p[j] && c <= p[j + 2]) hit = true; j += 2; }
                else if (p[j] == c) hit = true;
            }
            if (j >= p.size()) return false;   // unterminated class: no match
            i = j + 1; return hit != neg;
        };
        while (ti < t.size()) {
            if (pi < p.size() && p[pi] == '*') { star = pi++; mark = ti; continue; }
            if (pi < p.size()) {
                if (p[pi] == '?') { ++pi; ++ti; continue; }
                if (p[pi] == '[') { std::size_t i = pi; if (cls(i, t[ti])) { pi = i; ++ti; continue; } }
                else if (p[pi] == '\\' && pi + 1 < p.size()) { if (p[pi + 1] == t[ti]) { pi += 2; ++ti; continue; } }
                else if (p[pi] == t[ti]) { ++pi; ++ti; continue; }
            }
            if (star == std::string_view::npos) return false;
            pi = star + 1; ti = ++mark;
        }
        while (pi < p.size() && p[pi] == '*') ++pi;
        return pi == p.size();
    };
    auto split = [](std::string_view x) { std::vector<std::string_view> v; std::size_t a = 0; for (std::size_t i = 0; i <= x.size(); ++i) if (i == x.size() || x[i] == '/') { v.push_back(x.substr(a, i - a)); a = i + 1; } return v; };
    auto P = split(pat), S = split(s);
    std::function<bool(std::size_t, std::size_t)> m = [&](std::size_t i, std::size_t j) -> bool {
        if (i == P.size()) return j == S.size();
        if (P[i] == "**") { for (std::size_t k = j; k <= S.size(); ++k) if (m(i + 1, k)) return true; return false; }
        if (j == S.size()) return false;
        return seg_match(P[i], S[j]) && m(i + 1, j + 1);
    };
    return m(0, 0);
}
struct walk_entry { std::string path; bool is_dir = false, is_file = false, is_link = false; std::uint64_t size = 0; int depth = 0; };
struct walk_opts { int max_depth = -1; bool follow_symlinks = false, include_dirs = true, include_hidden = true; };
// Recursive listing below `root` (not including root). Unreadable directories are skipped.
inline result<std::vector<walk_entry>> walk(const std::string& root, walk_opts o = {}) {
    std::error_code ec;
    auto opt = stdfs::directory_options::skip_permission_denied;
    if (o.follow_symlinks) opt |= stdfs::directory_options::follow_directory_symlink;
    stdfs::recursive_directory_iterator it(u8path(root), opt, ec), end;
    if (ec) return {{}, from_ec(ec, "walk " + root)};
    std::vector<walk_entry> out;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        auto& e = *it;
        auto name = u8str(e.path().filename());
        if (!o.include_hidden && !name.empty() && name[0] == '.') { if (e.is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (o.max_depth >= 0 && it.depth() >= o.max_depth) it.disable_recursion_pending();
        walk_entry w; w.path = u8str(e.path()); w.depth = it.depth();
        w.is_link = e.is_symlink(ec); w.is_dir = e.is_directory(ec); w.is_file = e.is_regular_file(ec);
        if (w.is_file) { auto n = e.file_size(ec); w.size = ec ? 0 : std::uint64_t(n); ec.clear(); }
        if (w.is_dir && !o.include_dirs) continue;
        out.push_back(std::move(w));
    }
    return {std::move(out), {}};
}
// Paths matching `pattern` (relative patterns resolve against `base`; results keep the same form).
inline result<std::vector<std::string>> glob(const std::string& pattern, const std::string& base = ".") {
    std::string pat = pattern;
#if defined(FORGE_OS_WINDOWS)
    for (auto& c : pat) if (c == '\\') c = '/';
#endif
    // Longest literal directory prefix: walk only below it.
    std::size_t cut = 0;
    for (std::size_t i = 0; i < pat.size(); ++i) { if (pat[i] == '*' || pat[i] == '?' || pat[i] == '[') break; if (pat[i] == '/') cut = i + 1; }
    std::string prefix = pat.substr(0, cut);
    bool abs = !pat.empty() && (pat[0] == '/' || (pat.size() > 1 && pat[1] == ':'));
    std::string root = abs ? (prefix.empty() ? "/" : prefix) : (base + "/" + prefix);
    std::vector<std::string> out;
    if (cut == pat.size()) { if (exists(abs ? pat : base + "/" + pat)) out.push_back(pat); return {std::move(out), {}}; }
    bool deep = pat.find("**") != std::string::npos;
    int depth = 0; for (std::size_t i = cut; i < pat.size(); ++i) if (pat[i] == '/') ++depth;
    walk_opts wo; wo.max_depth = deep ? -1 : depth;
    auto w = walk(root, wo);
    if (!w) { if (!exists(root)) return {std::move(out), {}}; return {{}, w.err}; }
    std::string rootn = u8str(u8path(root).lexically_normal());
    for (auto& c : rootn) if (c == '\\') c = '/';   // Windows: lexically_normal gives '\\', entries below are compared with '/'
    if (!rootn.empty() && rootn.back() != '/') rootn += '/';
    for (auto& e : *w) {
        std::string p = u8str(u8path(e.path).lexically_normal());
        for (auto& c : p) if (c == '\\') c = '/';
        if (p.rfind(rootn, 0) != 0) continue;
        std::string rel = prefix + p.substr(rootn.size());
        if (glob_match(pat, rel)) out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return {std::move(out), {}};
}

// ---------------------------------------------------------------- advisory locks
class file_lock {
public:
    file_lock() = default;
    file_lock(const file_lock&) = delete; file_lock& operator=(const file_lock&) = delete;
    file_lock(file_lock&& o) noexcept : h_(std::exchange(o.h_, invalid())) {}
    file_lock& operator=(file_lock&& o) noexcept { if (this != &o) { unlock(); h_ = std::exchange(o.h_, invalid()); } return *this; }
    ~file_lock() { unlock(); }
    // Lock `path` (created if missing). exclusive=false takes a shared lock. wait=false fails at once if held.
    static result<file_lock> acquire(const std::string& path, bool exclusive = true, bool wait = true) {
        file_lock l;
#if defined(FORGE_OS_POSIX)
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return {{}, last_error("lock " + path)};
        int op = (exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB);
        while (::flock(fd, op) != 0) { if (errno == EINTR) continue; auto e = last_error("lock " + path); ::close(fd); return {{}, e}; }
        l.h_ = fd;
#else
        HANDLE h = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {{}, last_error("lock " + path)};
        OVERLAPPED ov{};
        DWORD fl = (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0) | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
        if (!LockFileEx(h, fl, 0, MAXDWORD, MAXDWORD, &ov)) { auto e = last_error("lock " + path); CloseHandle(h); return {{}, e}; }
        l.h_ = h;
#endif
        return {std::move(l), {}};
    }
    bool held() const { return h_ != invalid(); }
    void unlock() {
        if (!held()) return;
#if defined(FORGE_OS_POSIX)
        ::flock(h_, LOCK_UN); ::close(h_);
#else
        OVERLAPPED ov{}; UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov); CloseHandle(h_);
#endif
        h_ = invalid();
    }
private:
#if defined(FORGE_OS_POSIX)
    using handle = int; static constexpr handle invalid() { return -1; }
#else
    using handle = HANDLE; static handle invalid() { return INVALID_HANDLE_VALUE; }
#endif
    handle h_ = invalid();
};

// ---------------------------------------------------------------- disk usage and mounts
struct disk_usage_t { std::uint64_t total = 0, free = 0, available = 0; };
inline result<disk_usage_t> disk_usage(const std::string& p) {
    std::error_code ec; auto s = stdfs::space(u8path(p), ec);
    if (ec) return {{}, from_ec(ec, "disk usage " + p)};
    return {{std::uint64_t(s.capacity), std::uint64_t(s.free), std::uint64_t(s.available)}, {}};
}
// /proc/self/mounts escapes space, tab, newline and backslash as \ooo; anything that is not three octal digits
// stays literal (never throws).
inline std::string mount_unescape(const std::string& s) {
    auto oct = [](char c) { return c >= '0' && c <= '7'; };
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && oct(s[i + 1]) && oct(s[i + 2]) && oct(s[i + 3])) {
            o += char(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0')); i += 3;
        } else o += s[i];
    }
    return o;
}
struct mount_info { std::string device, path, type, options; };
inline result<std::vector<mount_info>> mounts() {
    std::vector<mount_info> out;
#if defined(__linux__)
    std::ifstream f("/proc/self/mounts");
    if (!f) return {{}, last_error("/proc/self/mounts")};
    std::string dev, path, type, opts, a, b;
    while (f >> dev >> path >> type >> opts >> a >> b) out.push_back({mount_unescape(dev), mount_unescape(path), type, opts});
    return {std::move(out), {}};
#elif defined(FORGE_OS_WINDOWS)
    char buf[512]; DWORD n = GetLogicalDriveStringsA(sizeof buf, buf);
    for (char* p = buf; p < buf + n && *p; p += std::strlen(p) + 1) {
        char fsname[64] = {}; GetVolumeInformationA(p, nullptr, 0, nullptr, nullptr, nullptr, fsname, sizeof fsname);
        out.push_back({p, p, fsname, ""});
    }
    return {std::move(out), {}};
#elif defined(__APPLE__) || defined(__FreeBSD__)
    // getfsstat into our own buffer (getmntinfo's buffer is shared process state, so not thread-safe).
    int n = ::getfsstat(nullptr, 0, MNT_NOWAIT); if (n < 0) return {{}, last_error("getfsstat")};
    std::vector<struct statfs> v(std::size_t(n) + 8);
    n = ::getfsstat(v.data(), int(v.size() * sizeof(struct statfs)), MNT_NOWAIT); if (n < 0) return {{}, last_error("getfsstat")};
    for (int i = 0; i < n; ++i) {
        const auto& s = v[std::size_t(i)]; std::string o = (s.f_flags & MNT_RDONLY) ? "ro" : "rw";
        if (s.f_flags & MNT_NOSUID) o += ",nosuid";
        if (s.f_flags & MNT_NOEXEC) o += ",noexec";
        if (s.f_flags & MNT_SYNCHRONOUS) o += ",sync";
        if (s.f_flags & MNT_LOCAL) o += ",local";
        out.push_back({s.f_mntfromname, s.f_mntonname, s.f_fstypename, o});
    }
    return {std::move(out), {}};
#else
    return {{}, unsupported("mounts")};
#endif
}

// ---------------------------------------------------------------- private files and directories (FSS FIO-1 / FIO-2)
// check_private, ensure_private_dir, write_new_private_file, private_kind: <forge/os/private_files.hpp> (namespace
// forge::os, so headers below os::fs can use them), available here too.
using os::private_kind;
using os::check_private;
using os::ensure_private_dir;
using os::write_new_private_file;

// ---------------------------------------------------------------- temp files and atomic writes
// FIO-1. Names: prefix + 10 characters from std::random_device + suffix; a name that exists is never reused.
//   make_temp_dir   POSIX mkdir(2) 0700 (exclusive); Windows CreateDirectoryW with the private DACL (the user, SYSTEM,
//                   Administrators; not inherited), so the directory is private wherever temp_dir() points.
//   make_temp_file  POSIX O_CREAT|O_EXCL|O_NOFOLLOW, 0600; Windows CREATE_NEW with the private DACL. In a shared
//                   sticky /tmp the file itself is private and cannot be pre-created or redirected by a symlink;
//                   for a private directory as well, pass dir = a make_temp_dir() / ensure_private_dir() result.
// `dir` (default temp_dir()) is where they are created.
inline std::string random_suffix(std::size_t n = 10) {
    static const char a[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd; std::string s; for (std::size_t i = 0; i < n; ++i) s += a[rd() % (sizeof a - 1)];
    return s;
}
inline result<std::string> make_temp_dir(const std::string& prefix = "forge-", const std::string& dir = "") {
    const std::string base = dir.empty() ? temp_dir() : dir;
    for (int i = 0; i < 16; ++i) {
        auto p = u8str(u8path(base) / u8path(prefix + random_suffix()));
#if defined(FORGE_OS_POSIX)
        if (::mkdir(p.c_str(), 0700) == 0) return {p, {}};
        if (errno != EEXIST) return {{}, last_error("make_temp_dir " + p)};
#else
        detail_priv::private_sa sa(true);
        if (CreateDirectoryW(widen(p).c_str(), sa.get())) return {p, {}};
        if (GetLastError() != ERROR_ALREADY_EXISTS) return {{}, last_error("make_temp_dir " + p)};
#endif
    }
    return {{}, error{-1, "make_temp_dir: could not create a unique directory"}};
}
inline result<std::string> make_temp_file(const std::string& prefix = "forge-", const std::string& suffix = "", const std::string& dir = "") {
    const std::string base = dir.empty() ? temp_dir() : dir;
    for (int i = 0; i < 16; ++i) {
        auto p = u8str(u8path(base) / u8path(prefix + random_suffix() + suffix));
#if defined(FORGE_OS_POSIX)
        int fd = ::open(p.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) { ::close(fd); return {p, {}}; }
        if (errno != EEXIST) return {{}, last_error("make_temp_file")};
#else   // CREATE_NEW: exclusive like O_EXCL, so a name that appears between choosing and creating is never reused
        detail_priv::private_sa sa(false);
        HANDLE h = CreateFileW(widen(p).c_str(), GENERIC_READ | GENERIC_WRITE, 0, sa.get(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return {p, {}}; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return {{}, last_error("make_temp_file")};
#endif
    }
    return {{}, error{-1, "make_temp_file: could not create a unique file"}};
}
// Write to a sibling temp file, flush it to disk and rename over `path`: readers see the old or the new
// content, never a torn file. mode: POSIX permission bits (less the umask); on Windows a mode without any write
// bit makes the file read-only, the rest has no NTFS equivalent.
inline status write_file_atomic(const std::string& path, std::string_view data, unsigned mode = 0644) {
    std::string tmp = path + ".tmp-" + random_suffix(8);
#if defined(FORGE_OS_POSIX)
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) return {{}, last_error("write " + tmp)};
    std::size_t off = 0;
    while (off < data.size()) { auto n = ::write(fd, data.data() + off, data.size() - off); if (n < 0) { if (errno == EINTR) continue; auto e = last_error("write " + tmp); ::close(fd); ::unlink(tmp.c_str()); return {{}, e}; } off += std::size_t(n); }
    if (::fsync(fd)) { auto e = last_error("fsync " + tmp); ::close(fd); ::unlink(tmp.c_str()); return {{}, e}; }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str())) { auto e = last_error("rename " + path); ::unlink(tmp.c_str()); return {{}, e}; }
    // Make the rename itself durable: fsync the containing directory (best effort; some filesystems refuse).
    auto slash = path.find_last_of('/');
    std::string dir = slash == std::string::npos ? std::string(".") : slash == 0 ? std::string("/") : path.substr(0, slash);
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { (void)::fsync(dfd); ::close(dfd); }
    return {};
#else
    auto a = widen(tmp), b = widen(path);
    HANDLE h = CreateFileW(a.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, (mode & 0222) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {{}, last_error("write " + tmp)};
    auto fail = [&](const char* what) {
        auto e = last_error(std::string(what) + " " + tmp); CloseHandle(h);
        SetFileAttributesW(a.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(a.c_str()); return status{{}, e};
    };
    for (std::size_t off = 0; off < data.size();) {
        DWORD put = 0; if (!WriteFile(h, data.data() + off, file::chunk(data.size() - off), &put, nullptr)) return fail("write");
        off += put;
    }
    if (!FlushFileBuffers(h)) return fail("flush");   // the data is on disk before the rename can expose it
    CloseHandle(h);
    if (!MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        auto e = last_error("rename " + path); SetFileAttributesW(a.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(a.c_str()); return {{}, e};
    }
    return {};
#endif
}

} // namespace forge::os::fs
