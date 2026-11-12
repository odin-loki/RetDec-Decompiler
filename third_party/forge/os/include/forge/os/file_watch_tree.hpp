#pragma once
// forge::os recursive, debounced file watching. tree_watcher watches a whole directory tree: every directory is
// registered with the native file_watcher (inotify / kqueue), or the whole tree with one recursive watch where the
// backend has one (Windows ReadDirectoryChangesW on the root, macOS FSEvents: no descriptor per directory or file);
// new directories are picked
// up as they appear (their contents are reported as created, closing the mkdir-then-write race), and a
// per-path snapshot turns coarse "directory changed" notifications (kqueue) into exact created / modified /
// removed events. Where native watching is unavailable, or with force_polling, it rescans on each poll().
// A directory the native backend cannot take (kqueue descriptor cap, inotify watch limit, permissions) is polled
// instead, as is every directory after the kernel reports an event overflow (one full rescan). Directories are
// entered once by identity (device + inode, volume + file id on Windows), so symlink and junction loops end.
// Bursts are coalesced: poll() waits until the tree has been quiet for debounce_ms and reports each path once.
// Guide: docs/os-wrapper.md.
#include <forge/os/file_watch.hpp>
#include <forge/os/fs.hpp>
#include <map>
#include <set>

namespace forge::os {
// max_native_handles: kqueue descriptor cap for this watcher (0 = the backend default); past it, polling.
// fsevents: macOS watches the tree through one FSEvents stream (coalesced, ~50 ms latency); false uses kqueue (one
// descriptor per directory and file, immediate events). Ignored elsewhere.
struct tree_watch_opts { int debounce_ms = 50; bool force_polling = false, include_hidden = true; std::vector<std::string> ignore; std::size_t max_native_handles = 0;
                         bool fsevents = true; };

class tree_watcher {
public:
    static result<std::unique_ptr<tree_watcher>> open(const std::string& root, tree_watch_opts o = {}) {
        if (!stat(root).is_dir) return {{}, {2, "watch: not a directory " + root}};
        auto w = std::unique_ptr<tree_watcher>(new tree_watcher(root, std::move(o)));
        w->polling_ = w->opts_.force_polling;
        if (w->opts_.max_native_handles) w->native_.set_max_handles(w->opts_.max_native_handles);
        bool tree = true;
#if defined(FORGE_OS_MACOS)
        tree = w->opts_.fsevents;
#endif
        if (!w->polling_) { w->subtree_ = tree && w->native_.add_tree(w->root_); if (!w->subtree_ && !w->native_.add(w->root_)) w->polling_ = true; }
        w->scan_(w->root_, nullptr);
        return {std::move(w), {}};
    }
    const char* backend() const { return polling_ ? "polling" : polled_.empty() ? native_.backend() : "native+polling"; }
    std::size_t watched_dirs() const { return dirs_.size(); }
    std::size_t polled_dirs() const { return polling_ ? dirs_.size() : polled_.size(); }   // directories rescanned instead of natively watched
    // Wait up to timeout_ms (-1 forever) for a change, then debounce and return the coalesced events.
    std::vector<watch_event> poll(int timeout_ms) {
        std::map<std::string, watch_kind> acc; std::vector<std::string> order;
        auto take = [&](std::vector<watch_event> ev) {
            for (auto& e : ev) {
                auto it = acc.find(e.path);
                if (it == acc.end()) { acc.emplace(e.path, e.kind); order.push_back(e.path); continue; }
                auto& k = it->second;
                if (k == watch_kind::created && e.kind == watch_kind::removed) k = watch_kind::other;      // came and went: drop
                else if (k == watch_kind::created && e.kind == watch_kind::modified) {}                    // still "created"
                else if (k == watch_kind::other && e.kind == watch_kind::created) k = watch_kind::created;
                else if (k == watch_kind::removed && e.kind == watch_kind::created) k = watch_kind::modified; // replaced
                else k = e.kind;
            }
        };
        auto start = monotonic_ns();
        auto remaining = [&]() -> int { if (timeout_ms < 0) return -1; auto el = int((monotonic_ns() - start) / 1000000); return el >= timeout_ms ? 0 : timeout_ms - el; };
        // First batch: block until something happens (polling samples at most every 100 ms).
        for (;;) {
            bool sample = polling_ || !polled_.empty();
            take(step_(sample ? (timeout_ms < 0 ? 100 : std::min(remaining(), 100)) : remaining()));
            if (!acc.empty() || remaining() == 0) break;
        }
        // Then keep draining until the tree is quiet for debounce_ms (bounded at 20 rounds).
        for (int i = 0; !acc.empty() && opts_.debounce_ms > 0 && i < 20; ++i) {
            auto before = acc.size(); auto snap = acc;
            take(step_(opts_.debounce_ms));
            if (acc.size() == before && acc == snap) break;
        }
        std::vector<watch_event> out;
        for (auto& p : order) { auto k = acc[p]; if (k != watch_kind::other) out.push_back({p, k}); }
        return out;
    }
private:
    struct entry { bool is_dir = false; std::int64_t mtime = 0; std::uint64_t size = 0; };
    using file_id = std::pair<std::uint64_t, std::uint64_t>;   // (device, inode) / (volume serial, file index)
    tree_watcher(std::string root, tree_watch_opts o) : root_(normalize_(std::move(root))), opts_(std::move(o)) {}
    static std::string normalize_(std::string p) { while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) p.pop_back(); return p; }
    // Identity of what p resolves to (symlinks and junctions followed); {0, 0} when unknown.
    static file_id id_of_(const std::string& p) {
#if defined(FORGE_OS_WINDOWS)
        HANDLE h = CreateFileW(widen(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return {0, 0};
        BY_HANDLE_FILE_INFORMATION i{}; bool ok = GetFileInformationByHandle(h, &i) != 0; CloseHandle(h);
        return ok ? file_id{std::uint64_t(i.dwVolumeSerialNumber) + 1, (std::uint64_t(i.nFileIndexHigh) << 32) | i.nFileIndexLow} : file_id{0, 0};
#else
        struct ::stat st{}; if (::stat(p.c_str(), &st)) return {0, 0};
        return {std::uint64_t(st.st_dev) + 1, std::uint64_t(st.st_ino)};
#endif
    }
    bool ignored_(const std::string& path) const {
        auto rel = path.size() > root_.size() ? path.substr(root_.size() + 1) : std::string();
        auto base = basename(path);
        if (!opts_.include_hidden && !base.empty() && base[0] == '.') return true;
        for (auto& g : opts_.ignore) if (fs::glob_match(g, rel) || fs::glob_match(g, base)) return true;
        return false;
    }
    static entry entry_of_(const std::string& p, bool& exists) { auto st = stat(p); exists = st.exists; return {st.is_dir, st.mtime_ns, st.size}; }
    // Record (and, when out != null, report as created) everything under dir; register dirs natively.
    void scan_(const std::string& dir, std::vector<watch_event>* out) {
        if (dirs_.count(dir)) return;
        auto id = id_of_(dir);
        if (id.first) {   // already entered through another path (a symlink / junction loop or alias): do not descend again
            auto [it, fresh] = ids_.emplace(id, dir);
            if (!fresh && it->second != dir) return;
        }
        dirs_.insert(dir);
        if (dir != root_ && !polling_ && !subtree_ && !native_.add(dir)) polled_.insert(dir);   // native watch refused: poll it
        auto ls = list_dir(dir); if (!ls) return;
        for (auto& e : ls.value) {
            auto p = join(dir, e.name); if (ignored_(p)) continue;
            bool ex = false; auto en = entry_of_(p, ex); if (!ex) continue;
            bool fresh = !snap_.count(p);
            snap_[p] = en;
            if (fresh && out) out->push_back({p, watch_kind::created});
            if (!en.is_dir) watch_file_(p, dir);
            else scan_(p, out);
        }
    }
    void watch_file_(const std::string& p, const std::string& dir) {   // kqueue per-file handle; its directory is polled when refused
        if (polling_ || subtree_ || polled_.count(dir)) return;           // a recursive watch already sees in-place writes
        if (!native_.add_file(p)) polled_.insert(dir);
    }
    // Compare the snapshot of `dir`'s immediate children against the disk.
    void diff_dir_(const std::string& dir, std::vector<watch_event>& out) {
        std::set<std::string> seen;
        if (auto ls = list_dir(dir)) for (auto& e : ls.value) {
            auto p = join(dir, e.name); if (ignored_(p)) continue; seen.insert(p); check_(p, out);
        }
        auto pre = dir + "/";
        std::vector<std::string> lost;   // collect first: gone_() erases whole subtrees from snap_
        for (auto it = snap_.lower_bound(pre); it != snap_.end() && it->first.compare(0, pre.size(), pre) == 0; ++it)
            if (it->first.find('/', pre.size()) == std::string::npos && !seen.count(it->first)) lost.push_back(it->first);
        for (auto& p : lost) gone_(p, out);
    }
    void forget_dir_(const std::string& d) {
        if (!polling_ && !subtree_) native_.remove(d);
        polled_.erase(d);
        for (auto it = ids_.begin(); it != ids_.end();) it = it->second == d ? ids_.erase(it) : std::next(it);
    }
    void gone_(const std::string& p, std::vector<watch_event>& out) {
        auto it = snap_.find(p); if (it == snap_.end()) return;
        bool was_dir = it->second.is_dir; snap_.erase(it); out.push_back({p, watch_kind::removed});
        if (was_dir) {
            if (dirs_.erase(p)) forget_dir_(p);
            auto pre = p + "/";
            for (auto j = snap_.lower_bound(pre); j != snap_.end() && j->first.compare(0, pre.size(), pre) == 0;) { out.push_back({j->first, watch_kind::removed}); j = snap_.erase(j); }
            for (auto d = dirs_.lower_bound(pre); d != dirs_.end() && d->compare(0, pre.size(), pre) == 0;) { forget_dir_(*d); d = dirs_.erase(d); }
        }
    }
    void check_(const std::string& p, std::vector<watch_event>& out) {
        if (ignored_(p)) return;
        bool ex = false; auto en = entry_of_(p, ex);
        auto it = snap_.find(p);
        if (!ex) { gone_(p, out); return; }
        if (it == snap_.end()) { snap_[p] = en; out.push_back({p, watch_kind::created}); if (en.is_dir) scan_(p, &out); else watch_file_(p, dirname(p)); return; }
        if (it->second.is_dir != en.is_dir) { gone_(p, out); snap_[p] = en; out.push_back({p, watch_kind::created}); if (en.is_dir) scan_(p, &out); return; }
        if (!en.is_dir && (it->second.mtime != en.mtime || it->second.size != en.size)) { it->second = en; out.push_back({p, watch_kind::modified}); }
    }
    void rescan_(const std::vector<std::string>& ds, std::vector<watch_event>& out) { for (auto& d : ds) if (dirs_.count(d)) diff_dir_(d, out); }
    std::vector<watch_event> step_(int timeout_ms) {
        std::vector<watch_event> out;
        if (polling_) {
            if (timeout_ms > 0) sleep_ns(std::uint64_t(timeout_ms) * 1000000ull);
            rescan_(std::vector<std::string>(dirs_.begin(), dirs_.end()), out);
            return out;
        }
        if (!polled_.empty() && (timeout_ms < 0 || timeout_ms > 100)) timeout_ms = 100;   // the polled part is sampled at least every 100 ms
        bool overflow = false;
        for (auto& e : native_.poll(timeout_ms)) {
            if (e.kind == watch_kind::overflow) { overflow = true; continue; }
            auto p = normalize_(e.path);
#if defined(FORGE_OS_MACOS)
            // FSEvents reports real paths; one the watcher could not map back below the root is unusable: rescan instead
            if (subtree_ && p != root_) {
                const std::string pre = root_ == "/" ? root_ : root_ + "/";
                if (p.compare(0, pre.size(), pre) != 0) { overflow = true; continue; }
            }
#endif
            if (dirs_.count(p) && e.kind != watch_kind::removed) diff_dir_(p, out);   // coarse (kqueue) dir event
            else check_(p, out);
        }
        if (overflow) rescan_(std::vector<std::string>(dirs_.begin(), dirs_.end()), out);   // events were lost: compare everything
        else if (!polled_.empty()) rescan_(std::vector<std::string>(polled_.begin(), polled_.end()), out);
        return out;
    }
    std::string root_; tree_watch_opts opts_; bool polling_ = false, subtree_ = false;
    file_watcher native_; std::map<std::string, entry> snap_; std::set<std::string> dirs_, polled_; std::map<file_id, std::string> ids_;
};
} // namespace forge::os
