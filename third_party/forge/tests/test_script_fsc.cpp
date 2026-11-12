// .fsc parse cache (script/src/fsc.cpp): round trip of every tree field, rejection of corrupt / stale /
// foreign / oversized files, trust rules (private directory or signature), atomic concurrent stores, and
// every tests/script/*.fs run uncached, cold and warm with identical results.
#include <forge/script/script.hpp>
#include "check.hpp"
#include "fsc.hpp"
#include "parser.hpp"
#include <forge/os/fs.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/secure_buffer.hpp>
#include "monocypher-ed25519.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif
using namespace forge;
namespace fs = std::filesystem;
namespace fsc = forge::script::fsc;
TEST_PROCESS_WORKER(script::run_process_worker_if_requested);   // sandbox_escape.fs starts a ProcessPool

namespace {
const std::string seed_a = "1111111111111111111111111111111111111111111111111111111111111111";
const std::string seed_b = "2222222222222222222222222222222222222222222222222222222222222222";
#if defined(_WIN32)
constexpr bool windows = true;
#else
constexpr bool windows = false;
#endif

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
void spit(const fs::path& p, std::string_view s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f.write(s.data(), std::streamsize(s.size())); }
std::span<const std::uint8_t> bytes_of(const std::vector<std::uint8_t>& v) { return {v.data(), v.size()}; }

struct tmpdir {
    fs::path p;
    tmpdir() { auto d = os::fs::make_temp_dir("forge-fsc-test-"); p = fs::path(d.value); }
    ~tmpdir() { std::error_code ec; fs::permissions(p, fs::perms::owner_all, fs::perm_options::add, ec); fs::remove_all(p, ec); }
};
std::vector<fs::path> files_in(const fs::path& d) {
    std::vector<fs::path> v; std::error_code ec;
    for (auto& e : fs::directory_iterator(d, ec)) v.push_back(e.path());
    return v;
}
std::size_t count_tmp(const fs::path& d) {
    std::size_t n = 0; for (auto& p : files_in(d)) if (p.filename().string().find(".tmp-") != std::string::npos) ++n; return n;
}

// Structural equality of two trees, every field the parser sets.
bool eq_ann(const script::TypeAnn& a, const script::TypeAnn& b) {
    if (a.name != b.name || a.nullable != b.nullable || a.args.size() != b.args.size()) return false;
    for (std::size_t i = 0; i < a.args.size(); ++i) if (!eq_ann(a.args[i], b.args[i])) return false;
    return true;
}
bool eq_annp(const std::shared_ptr<script::TypeAnn>& a, const std::shared_ptr<script::TypeAnn>& b) { return !a == !b && (!a || eq_ann(*a, *b)); }
bool eq_var(const script::VarRef& a, const script::VarRef& b) { return a.name == b.name && a.depth == b.depth && a.slot == b.slot; }
bool eq_fn(const script::FnDecl& a, const script::FnDecl& b);
bool eq_cls(const script::ClassDecl& a, const script::ClassDecl& b);
bool eq_node(const script::NodeP& a, const script::NodeP& b) {
    if (!a || !b) return !a && !b;
    if (a->k != b->k || a->line != b->line || a->col != b->col || a->s != b->s || a->i != b->i) return false;
    if (std::memcmp(&a->d, &b->d, sizeof a->d) || a->flag != b->flag || a->ref != b->ref || a->wb != b->wb) return false;
    if (!eq_var(a->var, b->var) || !eq_annp(a->type, b->type) || a->kids.size() != b->kids.size() || a->locals != b->locals) return false;
    for (std::size_t i = 0; i < a->kids.size(); ++i) if (!eq_node(a->kids[i], b->kids[i])) return false;
    if (!a->fn != !b->fn || (a->fn && !eq_fn(*a->fn, *b->fn))) return false;
    if (!a->cls != !b->cls || (a->cls && !eq_cls(*a->cls, *b->cls))) return false;
    return eq_node(a->extra, b->extra);
}
std::string fsrc(const script::FnDecl& f) { return f.src && f.src_b < f.src_e && f.src_e <= f.src->size() ? f.src->substr(f.src_b, f.src_e - f.src_b) : ""; }
bool eq_fn(const script::FnDecl& a, const script::FnDecl& b) {
    if (a.name != b.name || a.line != b.line || a.params.size() != b.params.size() || !eq_annp(a.ret, b.ret) || !eq_node(a.body, b.body)) return false;
    for (std::size_t i = 0; i < a.params.size(); ++i) {
        auto &p = a.params[i], &q = b.params[i];
        if (p.name != q.name || p.slot != q.slot || !eq_annp(p.type, q.type) || !eq_node(p.dflt, q.dflt) || p.by_ref != q.by_ref || p.out != q.out
            || p.conv != q.conv || p.tname != q.tname || p.cnst != q.cnst) return false;
    }
    return a.nslots == b.nslots && a.is_method == b.is_method && a.has_out == b.has_out && a.is_top == b.is_top && a.has_main == b.has_main
        && a.file == b.file && a.ret_name == b.ret_name && a.is_const == b.is_const && a.is_static == b.is_static && a.is_virtual == b.is_virtual
        && a.attrs == b.attrs && a.access == b.access && fsrc(a) == fsrc(b);
}
bool eq_cls(const script::ClassDecl& a, const script::ClassDecl& b) {
    if (a.name != b.name || a.base != b.base || a.interfaces != b.interfaces || a.is_struct != b.is_struct || a.is_dynamic != b.is_dynamic
        || a.is_interface != b.is_interface || a.fields.size() != b.fields.size() || a.methods.size() != b.methods.size()
        || a.required != b.required || a.statics != b.statics || !eq_var(a.var, b.var)) return false;
    for (std::size_t i = 0; i < a.fields.size(); ++i) if (a.fields[i].first != b.fields[i].first || !eq_node(a.fields[i].second, b.fields[i].second)) return false;
    for (std::size_t i = 0; i < a.methods.size(); ++i) if (!eq_fn(*a.methods[i], *b.methods[i])) return false;
    auto &m = *a.meta, &n = *b.meta;
    if (m.attrs != n.attrs || m.file != n.file || m.line != n.line || m.fields.size() != n.fields.size()) return false;
    for (std::size_t i = 0; i < m.fields.size(); ++i) {
        auto &x = m.fields[i], &y = n.fields[i];
        if (x.name != y.name || x.type != y.type || !eq_annp(x.ann, y.ann) || x.is_const != y.is_const || x.is_static != y.is_static
            || x.access != y.access || x.attrs != y.attrs || x.line != y.line) return false;
    }
    return true;
}

std::vector<fs::path> goldens() {
    std::vector<fs::path> v;
    for (auto& e : fs::directory_iterator("tests/script")) if (e.path().extension() == ".fs") v.push_back(e.path());
    std::sort(v.begin(), v.end());
    return v;
}

// A program exercising references, classes, lambdas, defaults, attributes and nesting (fields the cache must keep).
const char* rich_src = R"(
struct P { int x = 1; [[nodiscard]] int get() const { return x; } };
class B { public: virtual int f() { return 1; } };
class D : public B { public: int f() override { return 2; } };
int add(int a, int b = 2) { return a + b; }
void bump(int& r) { r += 1; }
int main() {
    int x = 1; int& r = x; r = 5; bump(x);
    auto l = [&](int k) { return k * x; };
    std::vector<int> v{1, 2, 3}; int s = 0; for (auto& e : v) s += e;
    D d; B& b = d; P p;
    std::cout << x << " " << l(2) << " " << s << " " << add(1) << " " << b.f() << " " << p.get() << std::endl;
    return 0;
}
)";

struct run_out { std::string out, status, error_type; bool ok = false; int exit_code = 0; };
run_out run_file(const fs::path& f, const std::string& cache_dir, bool vm, const std::string& seed = "", std::vector<std::string> trust = {},
                 bool allow_fs = true) {
    run_out r; script::options o; o.use_vm = vm; o.jit_threshold = 1; o.timeout_ms = 20000;
    o.fsc_cache_dir = cache_dir; o.fsc_sign_seed = seed; o.fsc_trusted_keys = std::move(trust);
    o.allow_fs = allow_fs; o.os_sandbox = script::options::os_sandbox_mode::off;
    o.out = [&](std::string_view s) { r.out += s; };
    script::engine e(o); auto x = e.run_file(f.string());
    r.status = e.cache_status(); r.ok = x.ok; r.error_type = x.error_type; r.exit_code = x.exit_code;
    return r;
}
// On Windows only signed caches load, so the engine-level tests sign there.
std::string platform_seed() { return windows ? seed_a : ""; }
} // namespace

TEST(roundtrip_keeps_every_field) {
    std::vector<std::pair<std::string, std::string>> progs{{"rich.fs", rich_src}};
    for (auto& f : goldens()) progs.push_back({f.string(), slurp(f)});
    for (auto& [name, src] : progs) {
        auto top = script::parse_program(src, name);
        auto key = fsc::key_of(src, name);
        auto enc = fsc::encode(*top, key);
        auto dec = fsc::decode(bytes_of(enc), key, {});
        CHECK_MSG(dec.has_value(), "%s: decode failed", name.c_str());
        if (!dec) continue;
        CHECK_MSG(eq_fn(*top, **dec), "%s: decoded tree differs from the parsed one", name.c_str());
        CHECK_MSG(fsc::encode(**dec, key) == enc, "%s: re-encoding differs", name.c_str());
    }
}

TEST(key_covers_source_name_and_version) {
    auto k = fsc::key_of("int x = 1;", "/a/x.fs");
    CHECK(k == fsc::key_of("int x = 1;", "/a/x.fs"));
    CHECK(k != fsc::key_of("int x = 2;", "/a/x.fs"));   // same size, different content
    CHECK(k != fsc::key_of("int x = 1;", "/b/x.fs"));
    CHECK(fsc::key_of("ab", "c") != fsc::key_of("a", "bc"));   // name/source boundary
    // A file encoded under another key is stale even when it is otherwise valid.
    auto top = script::parse_program("int x = 1;", "/a/x.fs");
    auto enc = fsc::encode(*top, k);
    std::string why;
    CHECK(!fsc::decode(bytes_of(enc), fsc::key_of("int x = 2;", "/a/x.fs"), {}, &why) && why.starts_with("stale"));
    // Version field: a file from another VM version is rejected before anything else is looked at.
    auto v = enc; v[4] ^= 1;
    CHECK(!fsc::decode(bytes_of(v), k, {}, &why) && why == "VM version mismatch");
}

TEST(corrupt_files_are_rejected) {
    const std::string src = rich_src, name = "rich.fs";
    auto top = script::parse_program(src, name);
    auto key = fsc::key_of(src, name);
    for (bool sign : {false, true}) {
        std::array<std::uint8_t, 32> seed{}; seed.fill(7);
        auto enc = fsc::encode(*top, key, sign ? &seed : nullptr);
        fsc::trust t;
        if (sign) {   // trust the signer: read its public key from the file (after the payload checksum and flag)
            std::array<std::uint8_t, 32> pub{}; std::memcpy(pub.data(), enc.data() + enc.size() - 96, 32); t.pubkeys.push_back(pub);
        }
        CHECK(fsc::decode(bytes_of(enc), key, t).has_value());
        std::string why;
        // every truncation
        for (std::size_t n = 0; n < enc.size(); n += (n < 64 ? 1 : 7)) {
            std::vector<std::uint8_t> v(enc.begin(), enc.begin() + std::ptrdiff_t(n));
            CHECK_MSG(!fsc::decode(bytes_of(v), key, t), "truncated to %zu accepted", n);
        }
        // a single bit flipped anywhere
        for (std::size_t i = 0; i < enc.size(); i += (i < 64 ? 1 : 3)) {
            auto v = enc; v[i] ^= std::uint8_t(1u << (i % 8));
            CHECK_MSG(!fsc::decode(bytes_of(v), key, t, &why), "bit flip at %zu accepted", i);
        }
        // trailing bytes, wrong magic
        { auto v = enc; v.push_back(0); CHECK(!fsc::decode(bytes_of(v), key, t, &why) && why == "trailing bytes"); }
        { auto v = enc; v[0] = 'X'; CHECK(!fsc::decode(bytes_of(v), key, t, &why) && why == "not an .fsc file"); }
        // huge payload length
        for (std::uint64_t len : std::initializer_list<std::uint64_t>{enc.size(), ~0ull, 1ull << 40, 0ull}) {
            auto v = enc; for (int i = 0; i < 8; ++i) v[40 + std::size_t(i)] = std::uint8_t(len >> (8 * i));
            CHECK(!fsc::decode(bytes_of(v), key, t, &why));
        }
        // a payload bit flip is caught by the checksum, not by luck in the decoder
        { auto v = enc; v[60] ^= 0x10; CHECK(!fsc::decode(bytes_of(v), key, t, &why) && why.find("checksum") != std::string::npos); }
        if (sign) {   // signature checks: untrusted signer, tampered signature
            CHECK(!fsc::decode(bytes_of(enc), key, {}, &why) && why == "signed by an untrusted key");
            auto v = enc; v.back() ^= 1;
            CHECK(!fsc::decode(bytes_of(v), key, t, &why) && why == "bad signature");
        } else {
            fsc::trust req; req.require_signed = true;
            CHECK(!fsc::decode(bytes_of(enc), key, req, &why) && why.starts_with("unsigned cache rejected"));
        }
    }
    CHECK(!fsc::decode({}, key, {}));
}

TEST(payload_decoder_bounds) {
    // Random and mutated payloads go straight to the payload decoder (no checksum in the way): never crash.
    auto top = script::parse_program(rich_src, "rich.fs");
    auto enc = fsc::encode(*top, fsc::key_of(rich_src, "rich.fs"));
    std::vector<std::uint8_t> payload(enc.begin() + 48, enc.end() - 33);
    CHECK(fsc::decode_payload(bytes_of(payload)) != nullptr);
    std::mt19937 rng(42);
    for (int it = 0; it < 3000; ++it) {
        auto v = payload;
        for (int m = 0, nm = 1 + int(rng() % 6); m < nm; ++m) {
            switch (rng() % 4) {
            case 0: v[rng() % v.size()] ^= std::uint8_t(1u << (rng() % 8)); break;
            case 1: v[rng() % v.size()] = std::uint8_t(rng()); break;
            case 2: if (v.size() > 8) { auto p = rng() % (v.size() - 4); std::uint32_t x = rng() % 2 ? 0xffffffffu : rng(); std::memcpy(v.data() + p, &x, 4); } break;
            case 3: v.resize(rng() % v.size() + 1); break;
            }
        }
        if (auto f = fsc::decode_payload(bytes_of(v))) {   // whatever decodes re-encodes to a decodable payload
            fsc::key_t k{}; auto again = fsc::encode(*f, k);
            CHECK(fsc::decode(bytes_of(again), k, {}).has_value());
        }
    }
    // A back-reference to an object still being decoded (a cycle) is rejected: fn{ body: Block{ kids: [ref -> itself] } }.
    std::vector<std::uint8_t> p;
    auto u8 = [&](std::uint8_t x) { p.push_back(x); };
    auto u32 = [&](std::uint32_t x) { for (int i = 0; i < 4; ++i) u8(std::uint8_t(x >> (8 * i))); };
    auto u64 = [&](std::uint64_t x) { u32(std::uint32_t(x)); u32(std::uint32_t(x >> 32)); };
    u8(1); u32(0); u32(0); u32(0); u8(0);                         // fn: new, name "", line 0, 0 params, no ret
    u8(1); u32(std::uint32_t(script::NK::Block)); u32(0); u32(0); // body: new node, kind, line, col
    u32(0); u64(0); u64(0); u8(0); u8(0); u8(0);                  // s, i, d, flag, ref, wb
    u32(0); u32(0); u32(0); u8(0);                                // var, no type
    u32(1); u8(2); u32(2);                                        // one kid: back-reference to node id 2 (itself)
    std::string why;
    CHECK(fsc::decode_payload(bytes_of(p), &why) == nullptr && why.find("cycle") != std::string::npos);
    // Nesting beyond the limit is rejected, not a stack overflow.
    p.clear(); u8(1); u32(0); u32(0); u32(0); u8(0);
    for (int i = 0; i < 6000; ++i) { u8(1); u32(std::uint32_t(script::NK::Block)); u32(0); u32(0); u32(0); u64(0); u64(0); u8(0); u8(0); u8(0); u32(0); u32(0); u32(0); u8(0); u32(1); }
    CHECK(fsc::decode_payload(bytes_of(p), &why) == nullptr && why.find("too deep") != std::string::npos);
    // A count larger than the bytes left is rejected before anything is allocated.
    p.clear(); u8(1); u32(0); u32(0); u32(0xffffffffu);
    CHECK(fsc::decode_payload(bytes_of(p), &why) == nullptr && why.find("count exceeds") != std::string::npos);
}

TEST(engine_hit_skips_parsing) {
    tmpdir t; auto cache = (t.p / "c").string(); auto f = t.p / "a.fs";
    spit(f, "std::cout << \"A\" << std::endl;");
    auto r1 = run_file(f, cache, true, platform_seed());
    CHECK_MSG(r1.status.starts_with("miss (stored)"), "%s", r1.status.c_str());
    auto r2 = run_file(f, cache, true, platform_seed());
    CHECK_MSG(r2.status == "hit" && r2.out == "A\n", "%s / %s", r2.status.c_str(), r2.out.c_str());
    // Replace the entry with the tree of another program under A's key: a hit runs that tree, so parsing was skipped.
    auto name = os::fs::u8str(fs::weakly_canonical(f));
    auto key = fsc::key_of(slurp(f), name);
    auto other = script::parse_program("std::cout << \"B\" << std::endl;", name);
    std::array<std::uint8_t, 32> seed{}; seed.fill(0x11);
    auto enc = fsc::encode(*other, key, windows ? &seed : nullptr);
    auto path = fsc::path_for(cache, key);
    CHECK(fs::exists(path));
    spit(path, std::string_view(reinterpret_cast<const char*>(enc.data()), enc.size()));
    auto r3 = run_file(f, cache, true, platform_seed());
    CHECK_MSG(r3.status == "hit" && r3.out == "B\n", "%s / %s", r3.status.c_str(), r3.out.c_str());
}

TEST(engine_invalidation_and_recovery) {
    tmpdir t; auto cache = (t.p / "c").string(); auto f = t.p / "a.fs";
    spit(f, "std::cout << 1 << std::endl;");
    CHECK(run_file(f, cache, true, platform_seed()).status.starts_with("miss (stored)"));
    CHECK(run_file(f, cache, true, platform_seed()).status == "hit");
    // Same size, same mtime, different content: a different key, so a miss with the new output.
    auto mt = fs::last_write_time(f);
    spit(f, "std::cout << 2 << std::endl;"); fs::last_write_time(f, mt);
    auto r = run_file(f, cache, true, platform_seed());
    CHECK_MSG(r.status.starts_with("miss (stored)") && r.out == "2\n", "%s / %s", r.status.c_str(), r.out.c_str());
    // Corrupt the stored entry: the run still succeeds, reports why, and rewrites a good entry.
    auto name = os::fs::u8str(fs::weakly_canonical(f));
    auto path = fsc::path_for(cache, fsc::key_of(slurp(f), name));
    auto good = slurp(path);
    for (std::string bad : {std::string(), good.substr(0, good.size() / 2), std::string(good).replace(60, 1, "\x7f"), std::string("FSC1garbage")}) {
        spit(path, bad);
        auto x = run_file(f, cache, true, platform_seed());
        CHECK_MSG(x.ok && x.out == "2\n" && x.status.starts_with("miss (stored)"), "%s / %s", x.status.c_str(), x.out.c_str());
        CHECK(slurp(path) == good);
        CHECK(run_file(f, cache, true, platform_seed()).status == "hit");
    }
    // Oversized entries are not read.
    { std::error_code ec; fs::resize_file(path, fsc::max_file_bytes + 1, ec); CHECK(!ec); }
    auto big = run_file(f, cache, true, platform_seed());
    CHECK_MSG(big.status.find("too large") != std::string::npos && big.out == "2\n", "%s", big.status.c_str());
    // A syntax error is reported normally and nothing is stored for it.
    spit(f, "int x = ;");
    auto se = run_file(f, cache, true, platform_seed());
    CHECK(!se.ok && se.error_type == "SyntaxError" && se.status == "miss: syntax error");
    CHECK(count_tmp(cache) == 0);
}

TEST(engine_signatures) {
    tmpdir t; auto cache = (t.p / "c").string(); auto f = t.p / "a.fs";
    spit(f, "std::cout << 3 << std::endl;");
    CHECK(run_file(f, cache, false, seed_a).status.starts_with("miss (stored)"));
    CHECK(run_file(f, cache, false, seed_a).status == "hit");
    // Another signer that does not trust A: rejected, re-stored under its own signature.
    auto r = run_file(f, cache, false, seed_b);
    CHECK_MSG(r.status.find("untrusted key") != std::string::npos && r.out == "3\n", "%s", r.status.c_str());
    CHECK(run_file(f, cache, false, seed_b).status == "hit");
    // Required signatures reject an unsigned entry even in a private directory.
    if (!windows) {
        auto c2 = (t.p / "u").string();
        CHECK(run_file(f, c2, false).status.starts_with("miss (stored)"));
        CHECK(run_file(f, c2, false).status == "hit");
        script::options o; o.fsc_cache_dir = c2; o.fsc_require_signed = true; o.out = [](std::string_view) {};
        script::engine e(o); auto x = e.run_file(f.string());
        CHECK_MSG(x.ok && e.cache_status().find("signatures required") != std::string::npos, "%s", e.cache_status().c_str());
    }
}

TEST(windows_unsigned_never_stored) {
    if (!windows) return;
    tmpdir t; auto cache = (t.p / "c").string(); auto f = t.p / "a.fs";
    spit(f, "std::cout << 4 << std::endl;");
    auto r = run_file(f, cache, true);
    CHECK_MSG(r.out == "4\n" && r.status.find("not stored") != std::string::npos, "%s", r.status.c_str());
    CHECK(files_in(cache).empty());
}

TEST(cached_tree_still_runs_static_checks) {
    // Capabilities are not in the key: a tree cached by a run with fs allowed is still refused for a run without it.
    tmpdir t; auto cache = (t.p / "c").string(); auto f = t.p / "a.fs";
    spit(f, "auto s = forge::fs::read(\"" + f.generic_string() + "\"); std::cout << s.size() << std::endl;");
    auto a = run_file(f, cache, true, platform_seed(), {}, true);
    CHECK_MSG(a.ok && a.status.starts_with("miss (stored)"), "%s %s", a.status.c_str(), a.error_type.c_str());
    auto b = run_file(f, cache, true, platform_seed(), {}, false);
    CHECK_MSG(!b.ok && b.status == "hit" && (b.error_type == "StaticCheckError" || b.error_type == "PermissionError"), "%s %s", b.status.c_str(), b.error_type.c_str());
}

#if !defined(_WIN32)
TEST(posix_permissions_and_symlinks) {
    tmpdir t; auto cache = t.p / "c"; auto f = t.p / "a.fs";
    spit(f, "std::cout << 5 << std::endl;");
    CHECK(run_file(f, cache.string(), true).status.starts_with("miss (stored)"));
    struct stat ds{}; CHECK(::stat(cache.c_str(), &ds) == 0 && (ds.st_mode & 0777) == 0700);
    auto entries = files_in(cache); CHECK(entries.size() == 1);
    struct stat fs_{}; CHECK(::stat(entries[0].c_str(), &fs_) == 0 && (fs_.st_mode & 0777) == 0600);
    CHECK(fsc::dir_private(cache.string()));
    // Group-writable directory: unsigned entries neither load nor get stored.
    ::chmod(cache.c_str(), 0770);
    CHECK(!fsc::dir_private(cache.string()));
    auto r = run_file(f, cache.string(), true);
    CHECK_MSG(r.out == "5\n" && r.status.find("signatures required") != std::string::npos && r.status.find("not stored") != std::string::npos, "%s", r.status.c_str());
    // ... signed ones still work there.
    CHECK(run_file(f, cache.string(), true, seed_a).status.starts_with("miss (stored)"));
    CHECK(run_file(f, cache.string(), true, seed_a).status == "hit");
    ::chmod(cache.c_str(), 0700);
    // Group-writable entry: treated as untrusted.
    CHECK(run_file(f, cache.string(), true).status.starts_with("miss (stored)"));
    ::chmod(entries[0].c_str(), 0660);
    CHECK(run_file(f, cache.string(), true).status.find("signatures required") != std::string::npos);
    // A symlink in place of the entry is never followed (even to a valid file).
    auto real = t.p / "real.fsc"; fs::copy_file(entries[0], real);
    fs::remove(entries[0]); fs::create_symlink(real, entries[0]);
    auto s = run_file(f, cache.string(), true);
    CHECK_MSG(s.status.find("symlink") != std::string::npos && s.out == "5\n", "%s", s.status.c_str());
    // A directory in place of the entry: not a regular file.
    fs::remove(entries[0]); fs::create_directory(entries[0]);
    auto d = run_file(f, cache.string(), true);
    CHECK_MSG(d.status.find("not a regular file") != std::string::npos && d.out == "5\n", "%s", d.status.c_str());
    // A missing cache directory is created 0700 even below missing parents.
    auto deep = t.p / "x" / "y" / "z";
    CHECK(run_file(f, deep.string(), true).status.starts_with("miss (stored)"));
    CHECK(::stat(deep.c_str(), &ds) == 0 && (ds.st_mode & 0777) == 0700);
}
#endif

TEST(concurrent_stores_and_loads) {
    // Writers race on one entry while readers load it: a reader sees no file or a complete one, never a torn file.
    tmpdir t; auto dir = (t.p / "c").string();
    auto top = script::parse_program(rich_src, "rich.fs");
    auto key = fsc::key_of(rich_src, "rich.fs");
    std::array<std::uint8_t, 32> seed{}; seed.fill(9);
    auto enc = fsc::encode(*top, key, windows ? &seed : nullptr);
    fsc::trust tr; if (windows) { std::array<std::uint8_t, 32> pub{}; std::memcpy(pub.data(), enc.data() + enc.size() - 96, 32); tr.pubkeys.push_back(pub); }
    std::atomic<int> bad{0}, hits{0}, store_fail{0}, writers{4};
    std::vector<std::thread> th;
    for (int w = 0; w < 4; ++w) th.emplace_back([&] { for (int i = 0; i < 40; ++i) { std::string why; if (!fsc::store(dir, key, bytes_of(enc), windows, &why)) ++store_fail; } --writers; });
    for (int rd = 0; rd < 4; ++rd) th.emplace_back([&] {
        for (int i = 0; writers > 0 || i < 20; ++i) {
            std::string why;
            if (fsc::load(dir, key, tr, &why)) ++hits;
            else if (why != "no cache file" && why != "no cache directory" && why != "cache file busy") { ++bad; std::fprintf(stderr, "  torn/odd read: %s\n", why.c_str()); }
        }
    });
    for (auto& x : th) x.join();
    CHECK(bad == 0);
    CHECK(hits > 0);
    // Windows refuses to replace a file another thread has open; those stores fail cleanly (the entry is intact).
    CHECK(windows || store_fail == 0);
    CHECK(count_tmp(dir) == 0);
    CHECK(fsc::load(dir, key, tr).has_value());
}

TEST(golden_scripts_cold_and_warm) {
    // Every golden script: uncached, cold cache (stored) and warm cache (hit) give the same output and result, on both engines.
    tmpdir t;
    for (bool vm : {true, false}) {
        auto cache = (t.p / (vm ? "vm" : "tree")).string();
        for (auto& f : goldens()) {
            auto name = f.filename().string();
            std::fprintf(stderr, "== %s (%s)\n", name.c_str(), vm ? "vm" : "tree"); std::fflush(stderr);
            auto plain = run_file(f, "", vm), cold = run_file(f, cache, vm, platform_seed()), warm = run_file(f, cache, vm, platform_seed());
            CHECK_MSG(plain.status == "off", "%s: %s", name.c_str(), plain.status.c_str());
            CHECK_MSG(cold.status.starts_with("miss (stored)"), "%s: cold %s", name.c_str(), cold.status.c_str());
            CHECK_MSG(warm.status == "hit", "%s: warm %s", name.c_str(), warm.status.c_str());
            for (auto* x : {&cold, &warm}) {
                CHECK_MSG(x->out == plain.out && x->ok == plain.ok && x->error_type == plain.error_type && x->exit_code == plain.exit_code,
                          "%s: cached run differs (%s)\n--- plain\n%s--- cached\n%s", name.c_str(), x->status.c_str(), plain.out.c_str(), x->out.c_str());
            }
        }
    }
}

// FORGE_FSC_WRITE_CORPUS=<dir>: regenerate the fuzz seeds (fuzz/corpus/fsc) from rich_src and small goldens.
TEST(write_fuzz_corpus) {
    const char* dir = std::getenv("FORGE_FSC_WRITE_CORPUS");
    if (!dir) return;
    fs::create_directories(dir);
    std::array<std::uint8_t, 32> seed{}; seed.fill(0x5a);
    auto put = [&](const std::string& out, const std::string& src, const std::string& name, bool sign) {
        auto top = script::parse_program(src, name);
        auto enc = fsc::encode(*top, fsc::key_of(src, name), sign ? &seed : nullptr);
        spit(fs::path(dir) / out, std::string_view(reinterpret_cast<const char*>(enc.data()), enc.size()));
    };
    put("rich.fsc", rich_src, "rich.fs", false);
    put("rich_signed.fsc", rich_src, "rich.fs", true);
    put("tiny.fsc", "int x = 1; std::cout << x;", "t.fs", false);
    for (std::string g : {"arithmetic", "classes", "functions"}) put(g + ".fsc", slurp(fs::path("tests/script") / (g + ".fs")), g + ".fs", false);
}

// FSS CAP-1: under an active policy the cache directory goes through enforce(fs_read / fs_write); outside the
// policy's roots nothing is stored or loaded (audited), inside it works as before. The signing seed lives in a
// security::secure_bytes.
TEST(cache_dir_enforced_by_policy) {
    tmpdir in, out;
    auto src = std::string("int x = 1;");
    auto top = script::parse_program(src, "p.fs");
    auto key = fsc::key_of(src, "p.fs");
    security::secure_bytes<32> seed; std::memset(seed.data(), 0x11, 32);
    auto enc = fsc::encode(*top, key, seed);
    std::array<std::uint8_t, 32> raw{}; raw.fill(0x11);
    CHECK(enc == fsc::encode(*top, key, &raw));   // same signature from the locked seed
    fsc::trust t; { std::uint8_t sk[64], pub[32], s2[32]; std::memcpy(s2, raw.data(), 32); crypto_ed25519_key_pair(sk, pub, s2); std::array<std::uint8_t, 32> pk; std::memcpy(pk.data(), pub, 32); t.pubkeys.push_back(pk); }
    auto p = security::profile_defaults(security::profile::strict);
    p.fs.read = {in.p.string()}; p.fs.write = {in.p.string()}; p.audit.os = false; p.audit.file.clear();
    security::set_active_policy(p);
    std::string why;
    auto outside = (out.p / "c").string(), inside = (in.p / "c").string();
    CHECK(!fsc::store(outside, key, bytes_of(enc), true, &why));
    CHECK_MSG(why.find("denied by the security policy") != std::string::npos, "%s", why.c_str());
    CHECK(!fs::exists(outside));
    CHECK(!fsc::load(outside, key, t, &why) && why.find("denied") != std::string::npos);
    CHECK_MSG(fsc::store(inside, key, bytes_of(enc), true, &why), "%s", why.c_str());
    CHECK_MSG(fsc::load(inside, key, t, &why).has_value(), "%s", why.c_str());
    security::set_active_policy(security::profile_defaults(security::profile::none));
    CHECK_MSG(fsc::store(outside, key, bytes_of(enc), true, &why), "%s", why.c_str());   // no policy: unchanged
}
