#pragma once
// File watching: inotify (Linux/Android), kqueue EVFILT_VNODE (macOS/iOS/FreeBSD), ReadDirectoryChangesW (Windows),
// FSEvents (macOS add_tree).
// Watching a file watches its directory and filters by name, so atomic replace-by-rename (editors, os::rename,
// forge modelfile writers) is seen as a modification of that file.  add() uses kqueue on macOS: immediate,
// uncoalesced events.  kqueue directory watches rescan the directory on each change and report created/removed/
// replaced entries by name; in-place writes to an entry need add_file() (inotify and ReadDirectoryChangesW see them
// through the directory).
// kqueue holds one descriptor per watched path: add() fails with EMFILE past max_handles() (default half the
// RLIMIT_NOFILE soft limit, at most 8192) instead of starving the process; tree_watcher then polls that part.
// add_tree() on macOS opens one recursive FSEvents stream (CoreServices, late-bound; per-file events, 50 ms latency,
// delivered on a private dispatch queue and woken through the kqueue): no descriptor per path, but events are
// coalesced (one event may carry created+modified) and reported for the real path, mapped back to the root as given.
// Windows completes every directory through one I/O completion port (no 64-handle wait limit) and matches names
// case-insensitively.  When the kernel drops events (inotify IN_Q_OVERFLOW, a Windows buffer overflow, FSEvents
// MustScanSubDirs / UserDropped / KernelDropped) poll() reports watch_kind::overflow for the affected directory: rescan it.
#include <forge/os/detail.hpp>
#include <algorithm>
#include <map>
#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(FORGE_OS_KQUEUE)
#include <sys/resource.h>
#if defined(FORGE_OS_MACOS)
#include <climits>
#include <cstdlib>
#include <strings.h>
#endif
#endif

namespace forge::os {
enum class watch_kind { created, modified, removed, renamed, other, overflow };
inline const char* watch_kind_name(watch_kind k) {
    switch (k) { case watch_kind::created: return "created"; case watch_kind::modified: return "modified"; case watch_kind::removed: return "removed"; case watch_kind::renamed: return "renamed";
                 case watch_kind::overflow: return "overflow"; default: return "other"; }
}
struct watch_event { std::string path; watch_kind kind = watch_kind::other; };

class file_watcher {
public:
    file_watcher() {
#if defined(__linux__)
        fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#elif defined(FORGE_OS_KQUEUE)
        fd_ = ::kqueue(); if (fd_ >= 0) ::fcntl(fd_, F_SETFD, FD_CLOEXEC);
        rlimit rl{}; std::size_t soft = ::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY ? std::size_t(rl.rlim_cur) : 16384;
        max_handles_ = std::min<std::size_t>(8192, std::max<std::size_t>(16, soft / 2));
#elif defined(FORGE_OS_WINDOWS)
        iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
#endif
    }
    ~file_watcher() {
#if defined(FORGE_OS_WINDOWS)
        // Cancel everything, then wait for the cancellations to come back: the kernel writes into each buffer
        // until its packet is dequeued. Anything still outstanding after 2 s is leaked rather than freed under it.
        for (auto& d : dirs_) retire_(std::move(d));
        dirs_.clear();
        for (auto end = monotonic_ns() + 2000000000ull; !closing_.empty() && monotonic_ns() < end;) drain_(100, nullptr);
        for (auto& d : closing_) (void)d.release();
        if (iocp_) CloseHandle(iocp_);
#elif defined(FORGE_OS_KQUEUE)
#if defined(FORGE_OS_MACOS)
        for (auto& s : fse_) fse_close_(*s);   // before the kqueue closes: callbacks trigger it
        fse_.clear();
#endif
        for (auto& [id, w] : kw_) ::close(w.fd);
        if (fd_ >= 0) ::close(fd_);
#else
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    file_watcher(const file_watcher&) = delete; file_watcher& operator=(const file_watcher&) = delete;
    const char* backend() const {
#if defined(__linux__)
        return "inotify";
#elif defined(FORGE_OS_KQUEUE)
#if defined(FORGE_OS_MACOS)
        if (!fse_.empty()) return kw_.empty() ? "fsevents" : "fsevents+kqueue";
#endif
        return "kqueue";
#elif defined(FORGE_OS_WINDOWS)
        return "ReadDirectoryChangesW";
#else
        return "none";
#endif
    }
    // kqueue: cap on the descriptors this watcher holds (a no-op elsewhere).
    void set_max_handles(std::size_t n) {
#if defined(FORGE_OS_KQUEUE)
        max_handles_ = std::max<std::size_t>(1, n);
#else
        (void)n;
#endif
    }
    std::size_t max_handles() const {
#if defined(FORGE_OS_KQUEUE)
        return max_handles_;
#else
        return std::size_t(-1);
#endif
    }
    status add(const std::string& path) {
        auto st = stat(path); bool is_dir = st.is_dir;
        if (!st.exists && !stat(dirname(path)).is_dir) return {{}, {2, "watch: no such path " + path}};
        std::string dir = is_dir ? path : dirname(path), name = is_dir ? "" : basename(path);
#if defined(__linux__)
        if (fd_ < 0) return {{}, last_error("inotify_init1")};
        int wd = ::inotify_add_watch(fd_, dir.c_str(), IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);
        if (wd < 0) return {{}, last_error("inotify_add_watch " + dir)};
        targets_.push_back({wd, dir, name}); return {};
#elif defined(FORGE_OS_KQUEUE)
        if (fd_ < 0) return {{}, last_error("kqueue")};
        if (auto s = kq_add_(dir, true, dir, name); !s) return s;
        if (!is_dir && st.exists) kq_add_(path, false, dir, name);
        return {};
#elif defined(FORGE_OS_WINDOWS)
        if (!iocp_) return {{}, last_error("CreateIoCompletionPort")};
        for (auto& d : dirs_) if (!d->subtree && !d->dead && ieq_(d->dir, dir)) { d->names.push_back(name); return {}; }
        auto d = std::make_unique<wdir>(); d->dir = dir; d->names.push_back(name);
        if (auto e = open_(*d)) return {{}, e};
        dirs_.push_back(std::move(d)); return {};
#else
        return {{}, unsupported("file watching")};
#endif
    }
    // Watch a whole tree natively: Windows through one handle on its root (per-directory handles would keep a
    // deleted subdirectory delete-pending, still visible, until the watch closed); macOS through one recursive
    // FSEvents stream. Events name the changed entry anywhere below dir. Returns false where unsupported (or
    // CoreServices cannot be loaded), and the caller then watches each directory with add().
    bool add_tree(const std::string& dir) {
#if defined(FORGE_OS_WINDOWS)
        if (!iocp_) return false;
        auto d = std::make_unique<wdir>(); d->dir = dir; d->names.push_back({}); d->subtree = true;
        if (open_(*d)) return false;
        dirs_.push_back(std::move(d)); return true;
#elif defined(FORGE_OS_MACOS)
        if (fd_ < 0) return false;
        const auto& a = fse_api::get(); const auto& cf = detail::cf_api::get();
        if (!a.ok || !cf.ok || !cf.CFArrayCreate || !cf.kCFTypeArrayCallBacks) return false;
        if (!fse_armed_) {   // the callback wakes poll() through this user event on the kqueue (udata 0: no kw_ id)
            struct kevent ev; EV_SET(&ev, fse_ident_, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
            if (::kevent(fd_, &ev, 1, nullptr, 0, nullptr)) return false;
            fse_armed_ = true;
        }
        char real[PATH_MAX]; if (!::realpath(dir.c_str(), real)) return false;
        auto s = std::make_unique<fse_stream>(); s->root = dir; s->real = real; s->kq = fd_;
        while (s->root.size() > 1 && s->root.back() == '/') s->root.pop_back();
        const void* str = cf.str(s->real.c_str()); if (!str) return false;
        const void* vals[1] = {str};
        const void* paths = cf.CFArrayCreate(nullptr, vals, 1, cf.kCFTypeArrayCallBacks); cf.CFRelease(str);
        if (!paths) return false;
        fse_api::context ctx{0, s.get(), nullptr, nullptr, nullptr};
        // kFSEventStreamEventIdSinceNow; NoDefer | WatchRoot | FileEvents
        s->stream = a.create(nullptr, &fse_cb_, &ctx, paths, ~std::uint64_t(0), 0.05, 0x02u | 0x04u | 0x10u);
        cf.CFRelease(paths);
        if (!s->stream) return false;
        s->queue = a.queue_create("forge.file_watch.fsevents", nullptr);
        if (!s->queue) { a.release(s->stream); return false; }
        a.set_queue(s->stream, s->queue);
        if (!a.start(s->stream)) { a.invalidate(s->stream); a.release(s->stream); a.queue_release(s->queue); return false; }
        fse_.push_back(std::move(s)); return true;
#else
        (void)dir; return false;
#endif
    }
    // Watch one file's own contents where the backend needs per-file handles (kqueue: directory events do
    // not fire for in-place writes). A no-op elsewhere, where the parent directory's watch already covers it.
    status add_file(const std::string& path) {
#if defined(FORGE_OS_KQUEUE)
        if (fd_ < 0) return {{}, last_error("kqueue")};
        for (auto& [id, x] : kw_) if (!x.is_dir && x.path == path) return {};
        return kq_add_(path, false, dirname(path), basename(path));
#else
        (void)path; return {};
#endif
    }
    // Stop watching a path given to add()/add_file() (a directory also drops the watches of its entries).
    status remove(const std::string& path) {
        bool any = false;
#if defined(__linux__)
        std::vector<int> wds;
        for (auto it = targets_.begin(); it != targets_.end();) {
            bool hit = it->name.empty() ? it->dir == path : join(it->dir, it->name) == path;
            if (hit) { wds.push_back(it->wd); it = targets_.erase(it); any = true; } else ++it;
        }
        for (int wd : wds) { bool used = false; for (auto& t : targets_) used |= t.wd == wd; if (!used) ::inotify_rm_watch(fd_, wd); }
#elif defined(FORGE_OS_KQUEUE)
        for (auto it = kw_.begin(); it != kw_.end();) {
            auto& w = it->second;
            bool hit = w.path == path || w.dir == path || (!w.name.empty() && join(w.dir, w.name) == path);
            if (hit) { ::close(w.fd); it = kw_.erase(it); any = true; } else ++it;
        }
#if defined(FORGE_OS_MACOS)
        for (auto it = fse_.begin(); it != fse_.end();) {
            if ((*it)->root == path || (*it)->real == path) { fse_close_(**it); it = fse_.erase(it); any = true; } else ++it;
        }
#endif
#elif defined(FORGE_OS_WINDOWS)
        for (std::size_t i = 0; i < dirs_.size();) {
            auto& d = *dirs_[i];
            if (ieq_(d.dir, path)) { retire_(std::move(dirs_[i])); dirs_.erase(dirs_.begin() + std::ptrdiff_t(i)); any = true; continue; }
            if (!d.subtree && ieq_(d.dir, dirname(path))) {
                auto nm = basename(path);
                for (auto n = d.names.begin(); n != d.names.end();) if (!n->empty() && ieq_(*n, nm)) { n = d.names.erase(n); any = true; } else ++n;
                if (d.names.empty()) { retire_(std::move(dirs_[i])); dirs_.erase(dirs_.begin() + std::ptrdiff_t(i)); continue; }
            }
            ++i;
        }
#endif
        if (!any) return {{}, {2, "watch: not watched " + path}};
        return {};
    }
    // Wait up to timeout_ms (-1 forever, 0 poll) and return whatever arrived.
    std::vector<watch_event> poll(int timeout_ms) {
        std::vector<watch_event> out;
#if defined(__linux__)
        pollfd p{fd_, POLLIN, 0}; if (::poll(&p, 1, timeout_ms) <= 0) return out;
        alignas(inotify_event) char buf[16384];
        for (;;) {
            auto n = ::read(fd_, buf, sizeof buf); if (n <= 0) break;
            for (char* q = buf; q < buf + n;) {
                auto* e = reinterpret_cast<inotify_event*>(q); q += sizeof(inotify_event) + e->len;
                if (e->mask & IN_Q_OVERFLOW) {   // the kernel queue overflowed: every watched directory may have missed events
                    std::vector<std::string> ds; for (auto& t : targets_) if (std::find(ds.begin(), ds.end(), t.dir) == ds.end()) ds.push_back(t.dir);
                    for (auto& d : ds) out.push_back({d, watch_kind::overflow});
                    continue;
                }
                if (e->mask & IN_IGNORED) {   // the watch is gone (directory deleted, or inotify_rm_watch): forget it
                    for (auto it = targets_.begin(); it != targets_.end();) it = it->wd == e->wd ? targets_.erase(it) : std::next(it);
                    continue;
                }
                std::string nm = e->len ? std::string(e->name) : "";
                for (auto& t : targets_) {
                    if (t.wd != e->wd || (!t.name.empty() && t.name != nm)) continue;
                    watch_kind k = e->mask & (IN_CREATE | IN_MOVED_TO) ? (t.name.empty() ? watch_kind::created : watch_kind::modified)
                                 : e->mask & (IN_DELETE | IN_DELETE_SELF) ? watch_kind::removed
                                 : e->mask & (IN_MOVED_FROM | IN_MOVE_SELF) ? watch_kind::renamed
                                 : e->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB) ? watch_kind::modified : watch_kind::other;
                    out.push_back({nm.empty() ? t.dir : join(t.dir, nm), k});
                }
            }
        }
#elif defined(FORGE_OS_KQUEUE)
        struct kevent ev[32]; timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        int n = ::kevent(fd_, nullptr, 0, ev, 32, timeout_ms < 0 ? nullptr : &ts);
        for (int i = 0; i < n; ++i) {
#if defined(FORGE_OS_MACOS)
            if (ev[i].filter == EVFILT_USER) { fse_drain_(out); continue; }
#endif
            auto id = std::uint64_t(reinterpret_cast<std::uintptr_t>(ev[i].udata));
            auto it = kw_.find(id); if (it == kw_.end()) continue;   // removed since the event was queued
            auto w = it->second;   // copy: kq_add_/erase below change kw_
            if (w.is_dir) {
                if (w.name.empty()) {   // a directory watch: rescan and diff against the snapshot, as inotify reports per entry
                    auto now = kq_scan_(w.dir); bool any = false;
                    for (auto& [nm, mt] : now) {
                        auto o = w.ents.find(nm);
                        if (o == w.ents.end()) { out.push_back({join(w.dir, nm), watch_kind::created}); any = true; }
                        else if (o->second != mt) { out.push_back({join(w.dir, nm), watch_kind::modified}); any = true; }
                    }
                    for (auto& [nm, mt] : w.ents) if (!now.count(nm)) { out.push_back({join(w.dir, nm), watch_kind::removed}); any = true; }
                    if (ev[i].fflags & (NOTE_DELETE | NOTE_RENAME)) { out.push_back({w.dir, ev[i].fflags & NOTE_DELETE ? watch_kind::removed : watch_kind::renamed}); any = true; }
                    if (!any) out.push_back({w.dir, watch_kind::modified});
                    if (ev[i].fflags & NOTE_DELETE) { ::close(w.fd); kw_.erase(id); }
                    else kw_[id].ents = std::move(now);
                    continue;
                }
                // directory changed: did our file appear / get replaced?
                std::string p = join(w.dir, w.name); auto st = stat(p);
                auto drop_file = [&] { for (auto j = kw_.begin(); j != kw_.end();) if (!j->second.is_dir && j->second.path == p) { ::close(j->second.fd); j = kw_.erase(j); } else ++j; };
                bool tracked = false; for (auto& [k, x] : kw_) if (!x.is_dir && x.path == p) tracked = true;
                if (st.exists && (!tracked || st.mtime_ns != w.mtime)) {
                    drop_file(); kq_add_(p, false, w.dir, w.name);
                    if (auto j = kw_.find(id); j != kw_.end()) j->second.mtime = st.mtime_ns;
                    out.push_back({p, tracked ? watch_kind::modified : watch_kind::created});
                } else if (!st.exists && tracked) { drop_file(); out.push_back({p, watch_kind::removed}); }
            } else {
                auto f = ev[i].fflags;
                out.push_back({w.path, f & NOTE_DELETE ? watch_kind::removed : f & NOTE_RENAME ? watch_kind::renamed : watch_kind::modified});
                if (f & (NOTE_DELETE | NOTE_RENAME)) { ::close(w.fd); kw_.erase(id); }   // the vnode is gone from this path
            }
        }
#elif defined(FORGE_OS_WINDOWS)
        if (dirs_.empty() && closing_.empty()) { if (timeout_ms > 0) sleep_ns(std::uint64_t(timeout_ms) * 1000000ull); return out; }
        drain_(timeout_ms < 0 ? INFINITE : DWORD(timeout_ms), &out);
#else
        (void)timeout_ms;
#endif
        return out;
    }
private:
#if defined(__linux__)
    struct target { int wd; std::string dir, name; };
    std::vector<target> targets_; int fd_ = -1;
#elif defined(FORGE_OS_KQUEUE)
    struct kwatch { int fd = -1; bool is_dir = false; std::string path, dir, name; std::int64_t mtime = 0; std::map<std::string, std::int64_t> ents; };
    // name -> mtime of a directory's entries (kqueue only says "the directory changed": diffing gives created/removed/replaced)
    static std::map<std::string, std::int64_t> kq_scan_(const std::string& dir) {
        std::map<std::string, std::int64_t> m; if (auto l = list_dir(dir)) for (auto& e : l.value) m[e.name] = stat(join(dir, e.name)).mtime_ns;
        return m;
    }
    status kq_add_(const std::string& p, bool is_dir, const std::string& dir, const std::string& name) {
        if (kw_.size() >= max_handles_) return {{}, {EMFILE, "kqueue: watch limit reached (" + std::to_string(max_handles_) + " descriptors) at " + p}};
#if defined(__APPLE__)
        int f = ::open(p.c_str(), O_EVTONLY | O_CLOEXEC);
#else
        int f = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
#endif
        if (f < 0) return {{}, last_error("open " + p)};
        std::uint64_t id = ++next_id_;
        kw_[id] = {f, is_dir, p, dir, name, is_dir && !name.empty() ? stat(join(dir, name)).mtime_ns : 0, is_dir && name.empty() ? kq_scan_(p) : std::map<std::string, std::int64_t>{}};
        struct kevent ev; EV_SET(&ev, f, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB, 0, reinterpret_cast<void*>(std::uintptr_t(id)));
        if (::kevent(fd_, &ev, 1, nullptr, 0, nullptr)) { auto e = last_error("kevent"); ::close(f); kw_.erase(id); return {{}, e}; }
        return {};
    }
    std::map<std::uint64_t, kwatch> kw_; std::uint64_t next_id_ = 0; std::size_t max_handles_ = 128; int fd_ = -1;
#if defined(FORGE_OS_MACOS)
    // ---- FSEvents (CoreServices, late-bound like the other Apple frameworks in forge::os; C ABI, no SDK headers)
    struct fse_api {
        // FSEventStreamCallback: (ConstFSEventStreamRef, info, numEvents, char** paths, const FSEventStreamEventFlags[], const FSEventStreamEventId[])
        using callback = void (*)(const void*, void*, std::size_t, void*, const std::uint32_t*, const std::uint64_t*);
        struct context { long version; void* info; const void* (*retain)(const void*); void (*release)(const void*); const void* (*describe)(const void*); };
        void* (*create)(const void*, callback, context*, const void*, std::uint64_t, double, std::uint32_t) = nullptr;
        void (*set_queue)(void*, void*) = nullptr;
        unsigned char (*start)(void*) = nullptr;
        void (*stop)(void*) = nullptr;
        void (*invalidate)(void*) = nullptr;
        void (*release)(void*) = nullptr;
        void* (*queue_create)(const char*, void*) = nullptr;               // libdispatch (libSystem)
        void (*sync_f)(void*, void*, void (*)(void*)) = nullptr;
        void (*queue_release)(void*) = nullptr;
        bool ok = false;
        static const fse_api& get() {
            static const fse_api a = [] {
                fse_api x;
                void* h = ::dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_LAZY);
                if (!h) h = ::dlopen("/System/Library/Frameworks/CoreServices.framework/Frameworks/FSEvents.framework/FSEvents", RTLD_LAZY);
                auto sym = [h](const char* n) -> void* { void* p = h ? ::dlsym(h, n) : nullptr; return p ? p : ::dlsym(RTLD_DEFAULT, n); };
                x.create = reinterpret_cast<decltype(x.create)>(sym("FSEventStreamCreate"));
                x.set_queue = reinterpret_cast<decltype(x.set_queue)>(sym("FSEventStreamSetDispatchQueue"));
                x.start = reinterpret_cast<decltype(x.start)>(sym("FSEventStreamStart"));
                x.stop = reinterpret_cast<decltype(x.stop)>(sym("FSEventStreamStop"));
                x.invalidate = reinterpret_cast<decltype(x.invalidate)>(sym("FSEventStreamInvalidate"));
                x.release = reinterpret_cast<decltype(x.release)>(sym("FSEventStreamRelease"));
                x.queue_create = reinterpret_cast<decltype(x.queue_create)>(::dlsym(RTLD_DEFAULT, "dispatch_queue_create"));
                x.sync_f = reinterpret_cast<decltype(x.sync_f)>(::dlsym(RTLD_DEFAULT, "dispatch_sync_f"));
                x.queue_release = reinterpret_cast<decltype(x.queue_release)>(::dlsym(RTLD_DEFAULT, "dispatch_release"));
                x.ok = x.create && x.set_queue && x.start && x.stop && x.invalidate && x.release && x.queue_create && x.sync_f && x.queue_release;
                return x;
            }();
            return a;
        }
    };
    // One stream; the callback runs on its serial dispatch queue and only queues raw (path, flags) pairs.
    struct fse_stream {
        std::string root, real;   // as given / canonical (FSEvents reports real paths: /private/var/..., not /var/...)
        void* stream = nullptr; void* queue = nullptr; int kq = -1;
        std::mutex m; std::vector<std::pair<std::string, std::uint32_t>> pending; bool dropped = false;
    };
    static constexpr std::uintptr_t fse_ident_ = 0x46534556;   // EVFILT_USER ident ("FSEV")
    static constexpr std::size_t fse_cap_ = 65536;              // queued events per stream; past it: overflow
    static void fse_cb_(const void*, void* info, std::size_t n, void* paths, const std::uint32_t* flags, const std::uint64_t*) {
        auto* s = static_cast<fse_stream*>(info); auto** p = static_cast<char**>(paths);
        {
            std::lock_guard g(s->m);
            for (std::size_t i = 0; i < n; ++i) {
                if (s->pending.size() >= fse_cap_) { s->dropped = true; break; }
                try { s->pending.emplace_back(p && p[i] ? p[i] : "", flags ? flags[i] : 0u); } catch (...) { s->dropped = true; break; }
            }
        }
        struct kevent ev; EV_SET(&ev, fse_ident_, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(s->kq, &ev, 1, nullptr, 0, nullptr);
    }
    static void fse_noop_(void*) {}
    static void fse_close_(fse_stream& s) {
        const auto& a = fse_api::get();
        if (s.stream) { a.stop(s.stream); a.invalidate(s.stream); }
        if (s.queue) a.sync_f(s.queue, nullptr, &fse_noop_);   // a callback already running finishes before s is freed
        if (s.stream) a.release(s.stream);
        if (s.queue) a.queue_release(s.queue);
        s.stream = s.queue = nullptr;
    }
    // A real path below s.real -> the same path below the root as the caller named it (APFS: case-insensitive).
    static std::string fse_map_(const fse_stream& s, std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        const auto& r = s.real;
        if (r.size() > 1 && p.size() >= r.size() && (p.size() == r.size() || p[r.size()] == '/') && ::strncasecmp(p.c_str(), r.c_str(), r.size()) == 0)
            return s.root + p.substr(r.size());
        return p;
    }
    void fse_drain_(std::vector<watch_event>& out) {
        for (auto& sp : fse_) {
            auto& s = *sp;
            std::vector<std::pair<std::string, std::uint32_t>> got; bool dropped = false;
            { std::lock_guard g(s.m); got.swap(s.pending); dropped = s.dropped; s.dropped = false; }
            if (dropped) out.push_back({s.root, watch_kind::overflow});
            for (auto& [raw, f] : got) {
                if (f & 0x18u) continue;                               // EventIdsWrapped, HistoryDone: bookkeeping
                std::string p = raw.empty() ? s.root : fse_map_(s, raw);
                if (f & 0x07u) { out.push_back({p, watch_kind::overflow}); continue; }   // MustScanSubDirs, UserDropped, KernelDropped
                if (f & 0x20u) { out.push_back({s.root, stat(s.root).exists ? watch_kind::renamed : watch_kind::removed}); continue; }   // RootChanged
                // Flags accumulate over the coalescing window (a file created and then written carries Created|Modified),
                // so the kind comes from the flags checked against what is on disk now.
                struct ::stat st{}; bool exists = ::lstat(p.c_str(), &st) == 0;
                watch_kind k = watch_kind::modified;
                if (f & 0x800u) k = exists ? watch_kind::created : watch_kind::renamed;              // ItemRenamed: moved in / out
                else if (f & 0x200u) k = exists ? (f & 0x100u ? watch_kind::created : watch_kind::modified) : watch_kind::removed;   // ItemRemoved (and re-created?)
                else if (f & 0x100u) k = exists ? watch_kind::created : watch_kind::removed;         // ItemCreated
                else if (!exists && (f & 0xff00u)) k = watch_kind::removed;
                out.push_back({p, k});
            }
        }
    }
    std::vector<std::unique_ptr<fse_stream>> fse_; bool fse_armed_ = false;
#endif
#elif defined(FORGE_OS_WINDOWS)
    struct wdir { std::string dir; std::vector<std::string> names; bool subtree = false, pending = false, dead = false; HANDLE h = INVALID_HANDLE_VALUE; OVERLAPPED ov{}; alignas(DWORD) std::uint8_t buf[32768]; };
    static bool ieq_(const std::string& a, const std::string& b) {   // NTFS names compare case-insensitively
        if (a == b) return true;
        auto wa = widen(a), wb = widen(b);
        return CompareStringOrdinal(wa.c_str(), int(wa.size()), wb.c_str(), int(wb.size()), TRUE) == CSTR_EQUAL;
    }
    error open_(wdir& d) {
        d.h = CreateFileW(widen(d.dir).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (d.h == INVALID_HANDLE_VALUE) return last_error("open dir " + d.dir);
        if (!CreateIoCompletionPort(d.h, iocp_, reinterpret_cast<ULONG_PTR>(&d), 0)) { auto e = last_error("CreateIoCompletionPort"); CloseHandle(d.h); d.h = INVALID_HANDLE_VALUE; return e; }
        if (!arm_(d)) { auto e = last_error("ReadDirectoryChangesW"); CloseHandle(d.h); d.h = INVALID_HANDLE_VALUE; return e; }
        return {};
    }
    static bool arm_(wdir& d) {
        d.ov = OVERLAPPED{};
        d.pending = ReadDirectoryChangesW(d.h, d.buf, sizeof d.buf, d.subtree ? TRUE : FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                          nullptr, &d.ov, nullptr) != 0;
        return d.pending;
    }
    // Close a watch; its buffer lives on in closing_ until the cancelled request's packet is dequeued.
    void retire_(std::unique_ptr<wdir> d) {
        if (d->h != INVALID_HANDLE_VALUE) { CancelIoEx(d->h, &d->ov); CloseHandle(d->h); d->h = INVALID_HANDLE_VALUE; }
        if (d->pending) closing_.push_back(std::move(d));
    }
    void drain_(DWORD timeout, std::vector<watch_event>* out) {
        OVERLAPPED_ENTRY es[64]; ULONG n = 0;
        if (!GetQueuedCompletionStatusEx(iocp_, es, 64, &n, timeout, FALSE)) return;
        for (ULONG i = 0; i < n; ++i) {
            auto* d = reinterpret_cast<wdir*>(es[i].lpCompletionKey);
            bool retired = false;
            for (auto it = closing_.begin(); it != closing_.end(); ++it) if (it->get() == d) { closing_.erase(it); retired = true; break; }
            if (retired || !out) continue;
            d->pending = false;
            DWORD got = es[i].dwNumberOfBytesTransferred; DWORD err = NO_ERROR;
            if (!GetOverlappedResult(d->h, &d->ov, &got, FALSE)) err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) continue;
            if ((err == NO_ERROR && got == 0) || err == ERROR_NOTIFY_ENUM_DIR) out->push_back({d->dir, watch_kind::overflow});   // buffer overflowed: changes were lost
            else if (err != NO_ERROR) { d->dead = true; continue; }   // e.g. the directory itself was deleted
            for (DWORD off = 0; err == NO_ERROR && got;) {
                auto* fi = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(d->buf + off);
                std::string nm = narrow({fi->FileName, fi->FileNameLength / sizeof(wchar_t)});
                if (d->subtree) for (auto& c : nm) if (c == '\\') c = '/';
                for (auto& want : d->names) if (want.empty() || ieq_(want, nm)) {
                    watch_kind k = fi->Action == FILE_ACTION_ADDED ? (want.empty() ? watch_kind::created : watch_kind::modified) : fi->Action == FILE_ACTION_REMOVED ? watch_kind::removed
                                 : fi->Action == FILE_ACTION_RENAMED_OLD_NAME ? watch_kind::renamed : fi->Action == FILE_ACTION_RENAMED_NEW_NAME ? (want.empty() ? watch_kind::created : watch_kind::modified) : watch_kind::modified;
                    out->push_back({join(d->dir, want.empty() ? nm : want), k}); break;
                }
                if (!fi->NextEntryOffset) break; off += fi->NextEntryOffset;
            }
            if (!arm_(*d)) d->dead = true;
        }
    }
    HANDLE iocp_ = nullptr; std::vector<std::unique_ptr<wdir>> dirs_, closing_;
#else
    int fd_ = -1;
#endif
};
} // namespace forge::os
