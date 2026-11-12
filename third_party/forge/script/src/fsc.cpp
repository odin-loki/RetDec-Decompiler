// ForgeScript .fsc cache encoder/decoder (see fsc.hpp).
#include "fsc.hpp"
#include <forge/hash/blake3.hpp>
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <forge/os/fs.hpp>
#include <forge/security/enforce.hpp>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forge::script::fsc {
namespace {
constexpr char magic[4] = {'F', 'S', 'C', '1'};
constexpr int max_depth = 4096;

struct W {
    std::vector<std::uint8_t> b;
    std::unordered_map<const void*, std::uint32_t> ids;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(std::uint8_t(v >> (8 * i))); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(std::uint8_t(v >> (8 * i))); }
    void i32(int v) { u32(std::uint32_t(v)); }
    void f64(double d) { std::uint64_t v; std::memcpy(&v, &d, 8); u64(v); }
    void str(const std::string& s) { u32(std::uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
    void strs(const std::vector<std::string>& v) { u32(std::uint32_t(v.size())); for (auto& s : v) str(s); }
    // 0 = null, 1 = new object follows, 2 + id = back-reference
    bool ref(const void* p) {
        if (!p) { u8(0); return false; }
        if (auto it = ids.find(p); it != ids.end()) { u8(2); u32(it->second); return false; }
        ids.emplace(p, std::uint32_t(ids.size() + 1)); u8(1); return true;
    }
    void var(const VarRef& v) { str(v.name); i32(v.depth); i32(v.slot); }
    void ann_val(const TypeAnn& t) { str(t.name); u8(t.nullable); u32(std::uint32_t(t.args.size())); for (auto& a : t.args) ann_val(a); }
    void ann(const std::shared_ptr<TypeAnn>& t) { if (ref(t.get())) ann_val(*t); }
    void node(const NodeP& n) {
        if (!ref(n.get())) return;
        u32(std::uint32_t(n->k)); i32(n->line); i32(n->col); str(n->s); u64(std::uint64_t(n->i)); f64(n->d); u8(n->flag); u8(n->ref); u8(n->wb);
        var(n->var); ann(n->type);
        u32(std::uint32_t(n->kids.size())); for (auto& k : n->kids) node(k);
        fn(n->fn); cls(n->cls); node(n->extra);
        u32(std::uint32_t(n->locals.size())); for (int l : n->locals) i32(l);
    }
    void fn(const std::shared_ptr<const FnDecl>& f) { if (ref(f.get())) fn_val(*f); }
    void fn_val(const FnDecl& f) {
        str(f.name); i32(f.line);
        u32(std::uint32_t(f.params.size()));
        for (auto& p : f.params) { str(p.name); i32(p.slot); ann(p.type); node(p.dflt); u8(p.by_ref); u8(p.out); u8(std::uint8_t(p.conv)); str(p.tname); u8(p.cnst); }
        ann(f.ret); node(f.body); i32(f.nslots);
        u8(f.is_method); u8(f.has_out); u8(f.is_top); u8(f.has_main); str(f.file); str(f.ret_name);
        u8(f.is_const); u8(f.is_static); u8(f.is_virtual); strs(f.attrs); str(f.access);
        str(f.src && f.src_b < f.src_e && f.src_e <= f.src->size() ? f.src->substr(f.src_b, f.src_e - f.src_b) : std::string());
    }
    void meta(const std::shared_ptr<ClassMeta>& m) {
        if (!ref(m.get())) return;
        strs(m->attrs); u32(std::uint32_t(m->fields.size()));
        for (auto& f : m->fields) { str(f.name); str(f.type); ann(f.ann); u8(f.is_const); u8(f.is_static); str(f.access); strs(f.attrs); i32(f.line); }
        str(m->file); i32(m->line);
    }
    void cls(const std::shared_ptr<const ClassDecl>& c) {
        if (!ref(c.get())) return;
        str(c->name); str(c->base); strs(c->interfaces); u8(c->is_struct); u8(c->is_dynamic); u8(c->is_interface);
        u32(std::uint32_t(c->fields.size())); for (auto& [n, d] : c->fields) { str(n); node(d); }
        u32(std::uint32_t(c->methods.size())); for (auto& m : c->methods) fn(m);
        strs(c->required); strs(c->statics); var(c->var); meta(c->meta);
    }
};

struct bad : std::runtime_error { using std::runtime_error::runtime_error; };

struct R {
    std::span<const std::uint8_t> b; std::size_t p = 0; int depth = 0;
    std::vector<std::shared_ptr<void>> objs;   // id - 1 -> object (type-checked by tag below)
    std::vector<int> kinds;                    // 1 TypeAnn 2 Node 3 FnDecl 4 ClassDecl 5 ClassMeta
    std::vector<char> done;                    // finished decoding: only these may be back-referenced (no cycles)
    void need(std::size_t n) { if (b.size() - p < n) throw bad("truncated"); }
    std::uint8_t u8() { need(1); return b[p++]; }
    bool bl() { auto v = u8(); if (v > 1) throw bad("bad bool"); return v; }
    std::uint32_t u32() { need(4); std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= std::uint32_t(b[p++]) << (8 * i); return v; }
    std::uint64_t u64() { need(8); std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= std::uint64_t(b[p++]) << (8 * i); return v; }
    int i32() { return int(u32()); }
    double f64() { std::uint64_t v = u64(); double d; std::memcpy(&d, &v, 8); return d; }
    // An element count; every element takes at least min_each encoded bytes, so a count never exceeds what is left.
    std::uint32_t count(std::size_t min_each = 1) { auto n = u32(); if (std::size_t(n) * min_each > b.size() - p) throw bad("count exceeds payload"); return n; }
    // Decode a list element by element: memory grows with the bytes actually consumed, not with the claimed count.
    template <class V, class F> void list(V& v, std::size_t min_each, F&& f) {
        auto n = count(min_each); v.clear(); v.reserve(std::min<std::size_t>(n, 256));
        for (std::uint32_t i = 0; i < n; ++i) { v.emplace_back(); f(v.back()); }
    }
    std::string str() { auto n = count(); std::string s(reinterpret_cast<const char*>(b.data() + p), n); p += n; return s; }
    std::vector<std::string> strs() { std::vector<std::string> v; list(v, 4, [&](std::string& s) { s = str(); }); return v; }
    struct guard { R& r; guard(R& x) : r(x) { if (++r.depth > max_depth) throw bad("nesting too deep"); } ~guard() { --r.depth; } };
    // returns: 0 null, 1 new (caller registers), 2 back-reference (out set)
    template <class T> int ref(int kind, std::shared_ptr<T>& out) {
        auto t = u8();
        if (t == 0) return 0;
        if (t == 1) return 1;
        if (t != 2) throw bad("bad ref tag");
        auto id = u32(); if (id == 0 || id > objs.size() || kinds[id - 1] != kind) throw bad("bad back-reference");
        if (!done[id - 1]) throw bad("back-reference cycle");
        out = std::static_pointer_cast<T>(objs[id - 1]); return 2;
    }
    template <class T> std::size_t reg(int kind, const std::shared_ptr<T>& o) { objs.push_back(o); kinds.push_back(kind); done.push_back(0); return objs.size() - 1; }
    VarRef var() { VarRef v; v.name = str(); v.depth = i32(); v.slot = i32(); return v; }
    TypeAnn ann_val() { guard g(*this); TypeAnn t; t.name = str(); t.nullable = bl(); list(t.args, 9, [&](TypeAnn& a) { a = ann_val(); }); return t; }
    std::shared_ptr<TypeAnn> ann() {
        std::shared_ptr<TypeAnn> t; if (ref(1, t) != 1) return t;
        t = std::make_shared<TypeAnn>(); auto id = reg(1, t); *t = ann_val(); done[id] = 1; return t;
    }
    NodeP node() {
        guard g(*this);
        std::shared_ptr<Node> n; if (ref(2, n) != 1) return n;
        n = std::make_shared<Node>(); auto id = reg(2, n);
        auto k = u32(); if (k > std::uint32_t(NK::Import)) throw bad("bad node kind"); n->k = NK(k);
        n->line = i32(); n->col = i32(); n->s = str(); n->i = std::int64_t(u64()); n->d = f64(); n->flag = bl(); n->ref = bl(); n->wb = bl();
        n->var = var(); n->type = ann();
        list(n->kids, 1, [&](NodeP& c) { c = node(); });
        n->fn = fn(); n->cls = cls(); n->extra = node();
        list(n->locals, 4, [&](int& l) { l = i32(); });
        done[id] = 1; return n;
    }
    std::shared_ptr<const FnDecl> fn() {
        guard g(*this);
        std::shared_ptr<FnDecl> f; if (ref(3, f) != 1) return f;
        f = std::make_shared<FnDecl>(); auto id = reg(3, f);
        f->name = str(); f->line = i32();
        list(f->params, 4, [&](Param& q) { q.name = str(); q.slot = i32(); q.type = ann(); q.dflt = node(); q.by_ref = bl(); q.out = bl(); q.conv = char(u8()); q.tname = str(); q.cnst = bl(); });
        f->ret = ann(); f->body = node(); f->nslots = i32();
        f->is_method = bl(); f->has_out = bl(); f->is_top = bl(); f->has_main = bl(); f->file = str(); f->ret_name = str();
        f->is_const = bl(); f->is_static = bl(); f->is_virtual = bl(); f->attrs = strs(); f->access = str();
        if (auto s = str(); !s.empty()) { f->src_e = s.size(); f->src = std::make_shared<const std::string>(std::move(s)); }
        if (f->nslots < 0 || f->nslots > (1 << 20)) throw bad("bad slot count");
        done[id] = 1; return f;
    }
    std::shared_ptr<ClassMeta> meta() {
        std::shared_ptr<ClassMeta> m; if (ref(5, m) != 1) return m;
        m = std::make_shared<ClassMeta>(); auto id = reg(5, m);
        m->attrs = strs();
        list(m->fields, 4, [&](MemberMeta& f) { f.name = str(); f.type = str(); f.ann = ann(); f.is_const = bl(); f.is_static = bl(); f.access = str(); f.attrs = strs(); f.line = i32(); });
        m->file = str(); m->line = i32(); done[id] = 1; return m;
    }
    std::shared_ptr<const ClassDecl> cls() {
        guard g(*this);
        std::shared_ptr<ClassDecl> c; if (ref(4, c) != 1) return c;
        c = std::make_shared<ClassDecl>(); auto id = reg(4, c);
        c->name = str(); c->base = str(); c->interfaces = strs(); c->is_struct = bl(); c->is_dynamic = bl(); c->is_interface = bl();
        list(c->fields, 5, [&](std::pair<std::string, NodeP>& fd) { fd.first = str(); fd.second = node(); });
        list(c->methods, 1, [&](std::shared_ptr<const FnDecl>& m) { m = fn(); });
        c->required = strs(); c->statics = strs(); c->var = var(); c->meta = meta();
        if (!c->meta) c->meta = std::make_shared<ClassMeta>();
        done[id] = 1; return c;
    }
};

void sig_digest(const key_t& key, const std::uint8_t pd[32], std::uint8_t out[44 + 32]) {
    std::memcpy(out, "forge-fsc-v1", 12); std::memcpy(out + 12, key.data(), 32); std::memcpy(out + 44, pd, 32);
}
#if !defined(_WIN32)
bool fd_private(int fd) { struct stat st{}; return ::fstat(fd, &st) == 0 && st.st_uid == ::geteuid() && !(st.st_mode & (S_IWGRP | S_IWOTH)); }
std::string err_str(const char* what) { return std::string(what) + ": " + std::strerror(errno); }
#endif
} // namespace

key_t key_of(std::string_view src, const std::string& file) {
    hash::blake3 h; h.update("forge-fsc\0", 10);
    std::uint8_t v[4]; for (int i = 0; i < 4; ++i) v[i] = std::uint8_t(vm_version >> (8 * i));
    h.update(v, 4); h.update(file.data(), file.size()); h.update("\0", 1); h.update(src.data(), src.size());
    auto d = h.final(); key_t k; std::memcpy(k.data(), d.data(), 32); return k;
}
std::string key_hex(const key_t& k) { hash::blake3::digest d; std::memcpy(d.data(), k.data(), 32); return hash::blake3::hex(d); }
std::string path_for(const std::string& dir, const key_t& k) { return os::fs::u8str(os::fs::u8path(dir) / (key_hex(k) + ".fsc")); }

namespace {
std::vector<std::uint8_t> encode_(const FnDecl& top, const key_t& key, const std::uint8_t* seed);
}
std::vector<std::uint8_t> encode(const FnDecl& top, const key_t& key, const std::array<std::uint8_t, 32>* seed) { return encode_(top, key, seed ? seed->data() : nullptr); }
std::vector<std::uint8_t> encode(const FnDecl& top, const key_t& key, const security::secure_bytes<32>& seed) { return encode_(top, key, seed.data()); }
namespace {
std::vector<std::uint8_t> encode_(const FnDecl& top, const key_t& key, const std::uint8_t* seed) {
    W pw; pw.u8(1); pw.ids.emplace(&top, 1u); pw.fn_val(top);
    W w; w.b.insert(w.b.end(), magic, magic + 4); w.u32(vm_version); w.b.insert(w.b.end(), key.begin(), key.end());
    w.u64(pw.b.size()); w.b.insert(w.b.end(), pw.b.begin(), pw.b.end());
    auto pd = hash::blake3::of(std::span<const std::uint8_t>(pw.b)); w.b.insert(w.b.end(), pd.begin(), pd.end());
    if (!seed) { w.u8(0); return w.b; }
    std::uint8_t msg[76]; sig_digest(key, pd.data(), msg);
    std::uint8_t sk[64], pub[32], sig[64], s2[32]; std::memcpy(s2, seed, 32);
    crypto_ed25519_key_pair(sk, pub, s2); crypto_ed25519_sign(sig, sk, msg, sizeof msg); crypto_wipe(sk, 64); crypto_wipe(s2, 32);
    w.u8(1); w.b.insert(w.b.end(), pub, pub + 32); w.b.insert(w.b.end(), sig, sig + 64);
    return w.b;
}
} // namespace

std::shared_ptr<const FnDecl> decode_payload(std::span<const std::uint8_t> payload, std::string* why) {
    try {
        R r{payload}; auto f = r.fn();
        if (!f || r.p != payload.size()) { if (why) *why = "malformed payload"; return nullptr; }
        if (!f->is_top) { if (why) *why = "malformed payload (not a program)"; return nullptr; }
        return f;
    } catch (const std::exception& e) { if (why) *why = std::string("malformed: ") + e.what(); return nullptr; }
}

std::optional<std::shared_ptr<const FnDecl>> decode(std::span<const std::uint8_t> bytes, const key_t& expect, const trust& t, std::string* why) {
    auto fail = [&](const std::string& m) -> std::optional<std::shared_ptr<const FnDecl>> { if (why) *why = m; return std::nullopt; };
    try {
        R h{bytes};
        h.need(4); if (std::memcmp(bytes.data(), magic, 4)) return fail("not an .fsc file"); h.p = 4;
        if (h.u32() != vm_version) return fail("VM version mismatch");
        h.need(32); if (std::memcmp(bytes.data() + h.p, expect.data(), 32)) return fail("stale (source or name changed)"); h.p += 32;
        auto n = h.u64(); if (n > bytes.size() - h.p) return fail("truncated payload");
        auto payload = bytes.subspan(h.p, std::size_t(n)); h.p += std::size_t(n);
        h.need(32); const std::uint8_t* sum = bytes.data() + h.p; h.p += 32;
        auto pd = hash::blake3::of(payload);
        if (std::memcmp(pd.data(), sum, 32)) return fail("payload checksum mismatch (corrupt)");
        bool is_signed = h.bl();
        if (is_signed) {
            h.need(96); const std::uint8_t* pub = bytes.data() + h.p; const std::uint8_t* sig = pub + 32; h.p += 96;
            bool known = false; for (auto& k : t.pubkeys) if (!std::memcmp(k.data(), pub, 32)) known = true;
            if (!known) return fail("signed by an untrusted key");
            std::uint8_t msg[76]; sig_digest(expect, pd.data(), msg);
            if (crypto_ed25519_check(sig, pub, msg, sizeof msg) != 0) return fail("bad signature");
        } else if (t.require_signed) return fail("unsigned cache rejected (signatures required)");
        if (h.p != bytes.size()) return fail("trailing bytes");
        std::string w;
        auto f = decode_payload(payload, &w);
        if (!f) return fail(w);
        return f;
    } catch (const std::exception& e) { return fail(std::string("malformed: ") + e.what()); }
}

bool dir_private(const std::string& d) {
#if defined(_WIN32)
    (void)d; return false;
#else
    int fd = ::open(d.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (fd < 0) return false;
    bool p = fd_private(fd); ::close(fd); return p;
#endif
}

std::optional<std::shared_ptr<const FnDecl>> load(const std::string& dir, const key_t& key, const trust& t, std::string* why) {
    auto fail = [&](const std::string& m) -> std::optional<std::shared_ptr<const FnDecl>> { if (why) *why = m; return std::nullopt; };
    // FSS CAP-1: under an active policy the cache directory must be readable (fs.read / fs.write roots); a denied
    // cache is not used (audited) and the source is parsed as if there were none.
    if (auto s = security::try_enforce(security::capability::fs_read, dir); !s) return fail("cache directory denied by the security policy: " + s.err.what);
    std::string name = key_hex(key) + ".fsc", bytes;
    trust tt = t;
#if defined(_WIN32)
    // Share read/write/delete so a concurrent store can still rename over the entry while we read the old one;
    // FILE_FLAG_OPEN_REPARSE_POINT opens a symlink itself (then rejected) instead of following it.
    auto w = os::widen(path_for(dir, key));
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20 && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        auto e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return fail("no cache file");
        if (e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) return fail("cache file unreadable");
        Sleep(1);   // a rename over the entry is in flight (delete pending): try again briefly
    }
    if (h == INVALID_HANDLE_VALUE) return fail("cache file busy");
    BY_HANDLE_FILE_INFORMATION fi{};
    if (!GetFileInformationByHandle(h, &fi) || (fi.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h); return fail("cache entry is not a regular file");
    }
    std::uint64_t sz = (std::uint64_t(fi.nFileSizeHigh) << 32) | fi.nFileSizeLow;
    if (sz > max_file_bytes) { CloseHandle(h); return fail("cache file too large"); }
    bytes.resize(std::size_t(sz));
    std::size_t off = 0;
    while (off < bytes.size()) {
        DWORD got = 0;
        if (!ReadFile(h, bytes.data() + off, DWORD(bytes.size() - off), &got, nullptr) || got == 0) break;
        off += got;
    }
    CloseHandle(h); bytes.resize(off);
    tt.require_signed = true;   // no owner/ACL check on Windows: only signed files load
#else
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return fail("no cache directory");
    bool priv = fd_private(dfd);
    int fd = ::openat(dfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int e = errno; ::close(dfd);
    // O_NOFOLLOW on a symlink: ELOOP (Linux, macOS), EMLINK (FreeBSD), EFTYPE (NetBSD).
    bool link = e == ELOOP || e == EMLINK;
#ifdef EFTYPE
    link = link || e == EFTYPE;
#endif
    if (fd < 0) return fail(link ? "cache entry is a symlink" : "no cache file");
    struct stat fs{};
    if (::fstat(fd, &fs) != 0 || !S_ISREG(fs.st_mode)) { ::close(fd); return fail("cache entry is not a regular file"); }
    if (std::uint64_t(fs.st_size) > max_file_bytes) { ::close(fd); return fail("cache file too large"); }
    if (fs.st_uid != ::geteuid() || (fs.st_mode & (S_IWGRP | S_IWOTH))) priv = false;
    bytes.resize(std::size_t(fs.st_size));
    std::size_t off = 0;
    while (off < bytes.size()) {
        auto r = ::read(fd, bytes.data() + off, bytes.size() - off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        off += std::size_t(r);
    }
    ::close(fd); bytes.resize(off);
    if (!priv) tt.require_signed = true;
#endif
    return decode(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), key, tt, why);
}

bool store(const std::string& dir, const key_t& key, std::span<const std::uint8_t> bytes, bool is_signed, std::string* why) {
    auto fail = [&](const std::string& m) { if (why) *why = m; return false; };
    if (auto s = security::try_enforce(security::capability::fs_write, dir); !s) return fail("cache directory denied by the security policy: " + s.err.what);
#if defined(_WIN32)
    if (!is_signed) return fail("unsigned caches never load on Windows (set a signing seed)");
    std::error_code ec; std::filesystem::create_directories(os::fs::u8path(dir), ec);
    auto w = os::fs::write_file_atomic(path_for(dir, key), std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), 0600);
    if (!w) return fail(w.err.what);
    return true;
#else
    std::string name = key_hex(key) + ".fsc";
    {
        std::error_code ec; auto parent = std::filesystem::path(dir).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return fail(err_str("mkdir"));
    }
    int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return fail(err_str("open directory"));
    if (!is_signed && !fd_private(dfd)) { ::close(dfd); return fail("directory not private (must be ours and not group/other-writable); an unsigned cache would not load"); }
    std::string tmp; int fd = -1;
    for (int i = 0; i < 16 && fd < 0; ++i) {
        tmp = name + ".tmp-" + os::fs::random_suffix(10);
        fd = ::openat(dfd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST) break;
    }
    if (fd < 0) { auto e = err_str("create temp"); ::close(dfd); return fail(e); }
    std::string err;
    for (std::size_t off = 0; off < bytes.size();) {
        auto n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { err = err_str("write"); break; }
        off += std::size_t(n);
    }
    if (err.empty() && ::fsync(fd) != 0) err = err_str("fsync");
    ::close(fd);
    // rename is atomic: a concurrent reader sees the old file, the new file or none, never a torn one.
    if (err.empty() && ::renameat(dfd, tmp.c_str(), dfd, name.c_str()) != 0) err = err_str("rename");
    if (!err.empty()) ::unlinkat(dfd, tmp.c_str(), 0);
    ::close(dfd);
    return err.empty() ? true : fail(err);
#endif
}

} // namespace forge::script::fsc
