#pragma once
#include <forge/os/os.hpp>
#include <forge/core/mapped.hpp>
#include "forge/modelfile/entropy.hpp"
// .fpk container: one file holding manifest, blueprint, tokenizer, tensor index and tensor data.
//
//   [header 64B] [sections ... each aligned to 64B] [section table]
//   header : "FORGEPK1" | u32 version | u32 n_sections | u64 table_offset | u64 file_size | 32B reserved
//   table  : n x { char name[32]; u64 offset; u64 size; u8 blake3[32]; u8 pad[16] }  (96 B each)
//   "tensors" section is JSON: [{name, codec, shape, offset, bytes, blake3}], offsets relative to "data".
//   Every section and every tensor carries a BLAKE3 hash; readers verify on demand (`verify()`).
//   Optional encryption hook: the "data" section may be sealed with XChaCha20-Poly1305 (Monocypher; the
//   plan's AES-GCM was swapped for an audited, constant-time, dependency-free AEAD). An "encryption"
//   section records {alg, nonce, mac, key_id}; the tensor index is bound as associated data. Metadata stays
//   readable, so signing, inspection and verification of section hashes work without the key.
#include <forge/core/error.hpp>
#include <forge/core/tensor.hpp>
#include <forge/graph/codecs.hpp>
#include <forge/hash/blake3.hpp>
#include <forge/json/json.hpp>
#include <forge/security/enforce.hpp>
#include <forge/security/files.hpp>
#include "monocypher.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <map>

namespace forge::modelfile {

inline constexpr char magic[8] = {'F', 'O', 'R', 'G', 'E', 'P', 'K', '1'};
inline constexpr std::uint32_t format_version = 1;
// Minor revision of version 1 (manifest "forge_format_minor"): 1 adds the optional "experts" section (an index of
// stacked MoE expert tensors whose per-expert slices start on expert_alignment file offsets). Readers that predate it
// ignore the section; the tensors themselves are ordinary entries of "tensors".
inline constexpr std::uint32_t format_minor = 1;
inline constexpr std::size_t alignment = 64;
inline constexpr std::size_t expert_alignment = 4096;

struct tensor_entry {
    std::string name, codec;
    shape_t shape;
    std::uint64_t offset = 0, bytes = 0;
    std::string blake3;
};

class writer {
public:
    void set_section(std::string name, std::string bytes) { sections_[std::move(name)] = std::move(bytes); }
    void set_manifest(const json::value& m) { set_section("manifest", json::dump(m)); }
    // Encodes f32 data with the named codec (any of graph::codecs::all()).
    void add_tensor(std::string name, const shape_t& shape, std::span<const float> data, std::string_view codec = "f32") {
        auto* c = graph::codecs::find(codec);
        if (!c) raise(errc::unsupported, "unknown codec '{}'", codec);
        if (std::size_t(numel(shape)) != data.size()) raise(errc::shape_mismatch, "tensor {}: shape/data mismatch", name);
        if (c->block > 1 && data.size() % std::size_t(c->block)) c = graph::codecs::find("f16");  // fallback for odd sizes
        while (data_.size() % alignment) data_.push_back(0);
        tensor_entry e{std::move(name), std::string(c->name), shape, data_.size(), c->bytes(data.size()), {}};
        data_.resize(data_.size() + e.bytes);
        c->encode(data.data(), data.size(), data_.data() + e.offset);
        e.blake3 = hash::blake3::hex(hash::blake3::of({data_.data() + e.offset, e.bytes}));
        tensors_.push_back(std::move(e));
    }
    // Stores already-encoded blocks (e.g. Q8_0 / Q4_K straight from a GGUF file) without re-quantising.
    void add_raw(std::string name, const shape_t& shape, std::string_view codec, std::span<const std::uint8_t> bytes) {
        auto* c = graph::codecs::find(codec);
        if (!c) raise(errc::unsupported, "unknown codec '{}'", codec);
        if (c->bytes(std::size_t(numel(shape))) != bytes.size()) raise(errc::format_error, "tensor {}: {} bytes do not match {} {}", name, bytes.size(), codec, shape_str(shape));
        while (data_.size() % alignment) data_.push_back(0);
        tensor_entry e{std::move(name), std::string(c->name), shape, data_.size(), bytes.size(), {}};
        data_.insert(data_.end(), bytes.begin(), bytes.end());
        e.blake3 = hash::blake3::hex(hash::blake3::of(bytes));
        tensors_.push_back(std::move(e));
    }
    void write(const std::string& path) {
        json::array idx;
        for (auto& t : tensors_) {
            json::array sh; for (auto d : t.shape) sh.push_back(double(d));
            idx.push_back(json::object{{"name", t.name}, {"codec", t.codec}, {"shape", sh}, {"offset", double(t.offset)},
                                       {"bytes", double(t.bytes)}, {"blake3", t.blake3}});
        }
        sections_["tensors"] = json::dump(idx);
        // Lay out sections first, then stream them to disk: the tensor data is never copied into a second buffer,
        // so peak memory stays at roughly one copy of the encoded weights (matters for 8B+ models).
        struct ent { std::string name; const std::uint8_t* p; std::uint64_t off, size; hash::blake3::digest h; };
        std::vector<ent> table;
        std::uint64_t pos = 64;
        auto place = [&](const std::string& nm, const std::uint8_t* p, std::size_t n) {
            pos = (pos + alignment - 1) / alignment * alignment;
            table.push_back({nm, p, pos, n, hash::blake3::of({p, n})});
            pos += n;
        };
        for (auto& [nm, s] : sections_) place(nm, reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        place("data", data_.data(), data_.size());
        std::uint64_t toff = (pos + alignment - 1) / alignment * alignment;
        std::vector<std::uint8_t> tail;
        for (auto& e : table) {
            std::uint8_t rec[96] = {};
            if (e.name.size() >= 32) raise(errc::invalid_argument, "section name too long: {}", e.name);
            std::memcpy(rec, e.name.data(), e.name.size());
            std::memcpy(rec + 32, &e.off, 8); std::memcpy(rec + 40, &e.size, 8); std::memcpy(rec + 48, e.h.data(), 32);
            tail.insert(tail.end(), rec, rec + 96);
        }
        std::uint32_t n = std::uint32_t(table.size());
        std::uint64_t fsz = toff + tail.size();
        std::uint8_t head[64] = {};
        std::memcpy(head, magic, 8); std::memcpy(head + 8, &format_version, 4); std::memcpy(head + 12, &n, 4);
        std::memcpy(head + 16, &toff, 8); std::memcpy(head + 24, &fsz, 8);
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) raise(errc::io_error, "cannot write {}", path);
        std::uint64_t at = 0;
        static const std::uint8_t zeros[alignment] = {};
        auto emit = [&](const std::uint8_t* p, std::size_t k) {
            if (k && std::fwrite(p, 1, k, f) != k) { std::fclose(f); raise(errc::io_error, "short write {}", path); }
            at += k;
        };
        emit(head, 64);
        for (auto& e : table) { emit(zeros, std::size_t(e.off - at)); emit(e.p, e.size); }
        emit(zeros, std::size_t(toff - at));
        emit(tail.data(), tail.size());
        std::fclose(f);
    }
    std::size_t tensor_bytes() const { return data_.size(); }
    // Pre-size the data buffer so growth never holds two copies of the weights at once.
    void reserve(std::size_t bytes) { data_.reserve(bytes); }
private:
    std::map<std::string, std::string> sections_;
    std::vector<tensor_entry> tensors_;
    std::vector<std::uint8_t> data_;
};

// Streaming writer: tensors go to disk as they are added (the caller frees each one), so peak memory is one
// tensor, not the whole model. Layout: [header][data section][other sections][table]; the header is patched last.
class stream_writer {
public:
    explicit stream_writer(std::string path) : path_(std::move(path)), tmp_(path_ + ".part") {
        f_ = std::fopen(tmp_.c_str(), "wb");
        if (!f_) raise(errc::io_error, "cannot write {}", tmp_);
        std::uint8_t z[64] = {}; emit_(z, 64);
    }
    stream_writer(const stream_writer&) = delete;
    ~stream_writer() { if (f_) { std::fclose(f_); std::remove(tmp_.c_str()); } }
    void set_section(std::string name, std::string bytes) { sections_[std::move(name)] = std::move(bytes); }
    void set_manifest(const json::value& m) { set_section("manifest", json::dump(m)); }
    void add_tensor(std::string name, const shape_t& shape, std::span<const float> data, std::string_view codec = "f32", std::size_t align = alignment) {
        auto* c = graph::codecs::find(codec);
        if (!c) raise(errc::unsupported, "unknown codec '{}'", codec);
        if (std::size_t(numel(shape)) != data.size()) raise(errc::shape_mismatch, "tensor {}: shape/data mismatch", name);
        if (c->block > 1 && data.size() % std::size_t(c->block)) c = graph::codecs::find("f16");
        std::vector<std::uint8_t> buf(c->bytes(data.size()));
        c->encode(data.data(), data.size(), buf.data());
        put_(std::move(name), std::string(c->name), shape, buf, align);
    }
    // `align` (a multiple of 64) applies to the tensor's file offset: expert_alignment for stacked MoE experts.
    void add_raw(std::string name, const shape_t& shape, std::string_view codec, std::span<const std::uint8_t> bytes, std::size_t align = alignment) {
        auto* c = graph::codecs::find(codec);
        if (!c) raise(errc::unsupported, "unknown codec '{}'", codec);
        if (c->bytes(std::size_t(numel(shape))) != bytes.size()) raise(errc::format_error, "tensor {}: {} bytes do not match {} {}", name, bytes.size(), codec, shape_str(shape));
        put_(std::move(name), std::string(c->name), shape, bytes, align);
    }
    std::size_t tensor_bytes() const { return dlen_; }
    // the most recently added tensor (offset relative to the data section, which starts at file offset 64)
    const tensor_entry& last() const { if (tensors_.empty()) raise(errc::invalid_argument, "no tensor written"); return tensors_.back(); }
    void finish() {
        json::array idx;
        for (auto& t : tensors_) {
            json::array sh; for (auto d : t.shape) sh.push_back(double(d));
            idx.push_back(json::object{{"name", t.name}, {"codec", t.codec}, {"shape", sh}, {"offset", double(t.offset)},
                                       {"bytes", double(t.bytes)}, {"blake3", t.blake3}});
        }
        sections_["tensors"] = json::dump(idx);
        struct ent { std::string name; std::uint64_t off, size; hash::blake3::digest h; };
        std::vector<ent> table{{"data", 64, dlen_, dhash_.final()}};
        for (auto& [nm, s] : sections_) {
            pad_();
            table.push_back({nm, at_, s.size(), hash::blake3::of({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()})});
            emit_(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
        }
        pad_();
        std::uint64_t toff = at_;
        for (auto& e : table) {
            std::uint8_t rec[96] = {};
            if (e.name.size() >= 32) raise(errc::invalid_argument, "section name too long: {}", e.name);
            std::memcpy(rec, e.name.data(), e.name.size());
            std::memcpy(rec + 32, &e.off, 8); std::memcpy(rec + 40, &e.size, 8); std::memcpy(rec + 48, e.h.data(), 32);
            emit_(rec, 96);
        }
        std::uint32_t n = std::uint32_t(table.size()); std::uint64_t fsz = at_;
        std::uint8_t head[64] = {};
        std::memcpy(head, magic, 8); std::memcpy(head + 8, &format_version, 4); std::memcpy(head + 12, &n, 4);
        std::memcpy(head + 16, &toff, 8); std::memcpy(head + 24, &fsz, 8);
        if (std::fseek(f_, 0, SEEK_SET) || std::fwrite(head, 1, 64, f_) != 64 || std::fclose(f_)) { f_ = nullptr; raise(errc::io_error, "cannot finish {}", tmp_); }
        f_ = nullptr;
        if (auto rs = forge::os::rename(tmp_, path_); !rs) { forge::os::remove(tmp_); raise(errc::io_error, "cannot replace {}: {}", path_, rs.err.what); }
    }
private:
    void emit_(const std::uint8_t* p, std::size_t k) {
        if (k && std::fwrite(p, 1, k, f_) != k) raise(errc::io_error, "short write {}", tmp_);
        at_ += k;
    }
    void pad_() { static const std::uint8_t z[alignment] = {}; emit_(z, std::size_t((alignment - at_ % alignment) % alignment)); }
    void put_(std::string name, std::string codec, const shape_t& shape, std::span<const std::uint8_t> b, std::size_t align = alignment) {
        static const std::uint8_t z[alignment] = {};
        if (align < alignment || align % alignment) raise(errc::invalid_argument, "tensor alignment {} is not a multiple of {}", align, alignment);
        std::size_t padn = (align - (64 + dlen_) % align) % align;   // data starts at file offset 64: align the file offset
        for (std::size_t left = padn; left;) { std::size_t n = std::min(left, alignment); emit_(z, n); dhash_.update(z, n); left -= n; }
        dlen_ += padn;
        tensor_entry e{std::move(name), std::move(codec), shape, dlen_, b.size(), hash::blake3::hex(hash::blake3::of(b))};
        emit_(b.data(), b.size()); dhash_.update(b.data(), b.size()); dlen_ += b.size();
        tensors_.push_back(std::move(e));
    }
    std::string path_, tmp_;
    std::FILE* f_ = nullptr;
    std::uint64_t at_ = 0, dlen_ = 0;
    hash::blake3 dhash_;
    std::map<std::string, std::string> sections_;
    std::vector<tensor_entry> tensors_;
};

namespace detail {
inline std::string hex(const std::uint8_t* p, std::size_t n) { static const char* d = "0123456789abcdef"; std::string s; for (std::size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; } return s; }
inline std::vector<std::uint8_t> unhex(std::string_view s) {
    auto v = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    if (s.size() % 2) raise(errc::invalid_argument, "bad hex");
    std::vector<std::uint8_t> o; for (std::size_t i = 0; i < s.size(); i += 2) { int a = v(s[i]), b = v(s[i + 1]); if (a < 0 || b < 0) raise(errc::invalid_argument, "bad hex"); o.push_back(std::uint8_t(a * 16 + b)); }
    return o;
}
inline std::string enc_key_id(const std::array<std::uint8_t, 32>& k) { auto d = hash::blake3::of({k.data(), k.size()}); return hex(d.data(), 8); }
inline std::string enc_ad(std::string_view tensors_json) { return std::string("forge-fpk-enc-v1\0", 17) + std::string(tensors_json); }
// Raw container write with sections copied verbatim.
inline void write_sections(const std::string& out, std::vector<std::pair<std::string, std::string>> secs) {
    std::sort(secs.begin(), secs.end());
    std::vector<std::uint8_t> o(64, 0);
    struct ent { std::string name; std::uint64_t off, size; hash::blake3::digest h; };
    std::vector<ent> table;
    for (auto& [nm, sec] : secs) {
        if (nm.size() >= 32) raise(errc::invalid_argument, "section name too long: {}", nm);
        while (o.size() % alignment) o.push_back(0);
        table.push_back({nm, o.size(), sec.size(), hash::blake3::of({reinterpret_cast<const std::uint8_t*>(sec.data()), sec.size()})});
        o.insert(o.end(), sec.begin(), sec.end());
    }
    while (o.size() % alignment) o.push_back(0);
    std::uint64_t toff = o.size();
    for (auto& e : table) {
        std::uint8_t rec[96] = {};
        std::memcpy(rec, e.name.data(), e.name.size());
        std::memcpy(rec + 32, &e.off, 8); std::memcpy(rec + 40, &e.size, 8); std::memcpy(rec + 48, e.h.data(), 32);
        o.insert(o.end(), rec, rec + 96);
    }
    std::uint32_t n = std::uint32_t(table.size()); std::uint64_t fsz = o.size();
    std::memcpy(o.data(), magic, 8); std::memcpy(o.data() + 8, &format_version, 4); std::memcpy(o.data() + 12, &n, 4);
    std::memcpy(o.data() + 16, &toff, 8); std::memcpy(o.data() + 24, &fsz, 8);
    auto tmp = out + ".tmp";
    { std::ofstream f(tmp, std::ios::binary); f.write(reinterpret_cast<const char*>(o.data()), std::streamsize(o.size())); if (!f) raise(errc::io_error, "cannot write {}", tmp); }
    if (auto rs = forge::os::rename(tmp, out); !rs) { forge::os::remove(tmp); raise(errc::io_error, "cannot replace {}: {}", out, rs.err.what); }
}
}
using model_key = std::array<std::uint8_t, 32>;
// Key from FORGE_MODEL_KEY (64 hex chars) or the file named by FORGE_MODEL_KEY_FILE (hex, whitespace ok).
inline std::optional<model_key> model_key_from_env() {
    std::string h;
    if (auto* e = std::getenv("FORGE_MODEL_KEY")) h = e;
    else if (auto* f = std::getenv("FORGE_MODEL_KEY_FILE")) {
        security::require_private_file("FORGE_MODEL_KEY_FILE", f, security::file_kind::secret);   // FSS-FIO-2
        std::ifstream in(f); std::string w; while (in >> w) h += w;
        if (!w.empty()) crypto_wipe(w.data(), w.size());
    }
    else return std::nullopt;
    auto b = detail::unhex(h); if (!h.empty()) crypto_wipe(h.data(), h.size());
    if (b.size() != 32) raise(errc::invalid_argument, "model key must be 32 bytes (64 hex chars)");
    model_key k; std::memcpy(k.data(), b.data(), 32); crypto_wipe(b.data(), b.size()); return k;
}

// Memory-mapped reader. Tensors are decoded to f32 on request (and cached by the caller).
class reader {
public:
    explicit reader(const std::string& path, std::optional<model_key> key = std::nullopt) : path_(path) {
        // FSS CAP-1: every .fpk opened (run, serve, inspect, dump, verify, sign, encrypt) passes enforce(fs_read)
        // under an active policy; no-op with profile none.
        security::enforce(security::capability::fs_read, path);
        map_ = forge::map_readonly(path, size_);
        try { init_(path, key); } catch (...) { forge::unmap_readonly(map_, size_); map_ = nullptr; throw; }   // the dtor does not run on a throwing ctor
    }
private:
    void init_(const std::string& path, std::optional<model_key> key) {
        if (size_ < 64) raise(errc::format_error, "{}: too small to be .fpk", path);
        if (std::memcmp(map_, magic, 8)) raise(errc::format_error, "{}: bad magic", path);
        std::uint32_t ver, n; std::uint64_t toff, fsz;
        std::memcpy(&ver, map_ + 8, 4); std::memcpy(&n, map_ + 12, 4); std::memcpy(&toff, map_ + 16, 8); std::memcpy(&fsz, map_ + 24, 8);
        if (ver != format_version) raise(errc::unsupported, "{}: format version {} not supported", path, ver);
        if (fsz != size_ || toff + std::uint64_t(n) * 96 > size_) raise(errc::format_error, "{}: truncated or corrupt", path);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto* r = map_ + toff + 96 * i;
            section s; s.name.assign(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 32));
            std::memcpy(&s.offset, r + 32, 8); std::memcpy(&s.size, r + 40, 8); std::memcpy(s.hash.data(), r + 48, 32);
            if (s.offset + s.size > size_) raise(errc::format_error, "{}: section {} out of bounds", path, s.name);
            sections_[s.name] = s;
        }
        if (has_section("encryption")) { auto k = key ? key : model_key_from_env(); if (k) decrypt(k); else locked_ = true; }
        auto idx = json::parse(section_text("tensors"));
        for (auto& t : idx.arr()) {
            tensor_entry e; e.name = t["name"].str(); e.codec = t["codec"].str();
            for (auto& d : t["shape"].arr()) e.shape.push_back(d.i());
            e.offset = std::uint64_t(t["offset"].num()); e.bytes = std::uint64_t(t["bytes"].num()); e.blake3 = t["blake3"].str();
            if (e.offset + e.bytes > sections_.at("data").size) raise(errc::format_error, "tensor {} out of bounds", e.name);
            tensors_[e.name] = e;
        }
    }
public:
    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;
    ~reader() { forge::unmap_readonly(map_, size_); }

    bool has_section(const std::string& n) const { return sections_.count(n) != 0; }
    std::span<const std::uint8_t> section_bytes(const std::string& n) const {
        auto it = sections_.find(n);
        if (it == sections_.end()) raise(errc::not_found, "{}: no section '{}'", path_, n);
        if (n == "data" && plain_) return {plain_->data(), plain_->size()};
        if (n == "data" && locked_) raise(errc::invalid_argument, "{}: model is encrypted (key id {}); set FORGE_MODEL_KEY or FORGE_MODEL_KEY_FILE",
                                           path_, json::parse(section_text("encryption")).str_or("key_id", "?"));
        return {map_ + it->second.offset, it->second.size};
    }
    // Section bytes exactly as stored (ciphertext for an encrypted data section).
    std::span<const std::uint8_t> raw_section_bytes(const std::string& n) const {
        auto it = sections_.find(n);
        if (it == sections_.end()) raise(errc::not_found, "{}: no section '{}'", path_, n);
        return {map_ + it->second.offset, it->second.size};
    }
    bool locked() const { return locked_; }
    bool encrypted() const { return plain_.has_value(); }
    std::string_view section_text(const std::string& n) const {
        auto b = section_bytes(n); return {reinterpret_cast<const char*>(b.data()), b.size()};
    }
    json::value manifest() const { return json::parse(section_text("manifest")); }
    const std::map<std::string, tensor_entry>& tensors() const { return tensors_; }
    const tensor_entry& entry(const std::string& n) const {
        auto it = tensors_.find(n);
        if (it == tensors_.end()) raise(errc::not_found, "{}: no tensor '{}'", path_, n);
        return it->second;
    }
    tensor load(const std::string& n) const {
        const auto& e = entry(n);
        auto* c = graph::codecs::find(e.codec);
        if (!c) raise(errc::unsupported, "tensor {}: unknown codec {}", n, e.codec);
        tensor t(e.shape);
        c->decode(section_bytes("data").data() + e.offset, t.size(), t.data.data());
        return t;
    }
    // Verifies every section hash and every tensor hash. Returns list of problems (empty == ok).
    std::vector<std::string> verify() const {
        std::vector<std::string> bad;
        for (auto& [n, s] : sections_)
            if (hash::blake3::of({map_ + s.offset, s.size}) != s.hash) bad.push_back("section " + n);
        if (locked_) return bad;                       // tensor hashes are over plaintext: need the key
        auto d = section_bytes("data");
        for (auto& [n, e] : tensors_)
            if (hash::blake3::hex(hash::blake3::of({d.data() + e.offset, e.bytes})) != e.blake3) bad.push_back("tensor " + n);
        return bad;
    }
    std::size_t file_size() const { return size_; }
    struct section { std::string name; std::uint64_t offset = 0, size = 0; hash::blake3::digest hash{}; };
    const std::map<std::string, section>& sections() const { return sections_; }
private:
    void decrypt(const std::optional<model_key>& key) {
        auto e = json::parse(section_text("encryption"));
        if (e.str_or("alg", "") != "xchacha20-poly1305") raise(errc::unsupported, "{}: unknown encryption {}", path_, e.str_or("alg", "?"));
        if (detail::enc_key_id(*key) != e.str_or("key_id", "")) raise(errc::integrity_error, "{}: wrong key (file wants key id {})", path_, e.str_or("key_id", "?"));
        auto nonce = detail::unhex(e["nonce"].str()), mac = detail::unhex(e["mac"].str());
        if (nonce.size() != 24 || mac.size() != 16) raise(errc::format_error, "{}: bad encryption header", path_);
        auto& d = sections_.at("data"); auto ad = detail::enc_ad(section_text("tensors"));
        plain_.emplace(d.size);
        if (crypto_aead_unlock(plain_->data(), mac.data(), key->data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(), map_ + d.offset, d.size))
            { plain_.reset(); raise(errc::integrity_error, "{}: decryption failed (tampered data or index)", path_); }
    }
    std::optional<std::vector<std::uint8_t>> plain_;
    bool locked_ = false;
    std::string path_;
    const std::uint8_t* map_ = nullptr;
    std::size_t size_ = 0;
    std::map<std::string, section> sections_;
    std::map<std::string, tensor_entry> tensors_;
};

// Seal the data section of `in` with `key`, writing `out`. Existing signatures are dropped (re-sign after).
inline void encrypt_file(const std::string& in, const std::string& out, const model_key& key) {
    std::vector<std::pair<std::string, std::string>> secs; std::string tj; std::vector<std::uint8_t> plain;
    {
        reader r(in);
        if (r.has_section("encryption")) raise(errc::invalid_argument, "{}: already encrypted", in);
        if (auto bad = r.verify(); !bad.empty()) raise(errc::format_error, "{}: refusing to encrypt a corrupt file ({})", in, bad.front());
        for (auto& [name, s] : r.sections()) {
            if (name == "signature") continue;
            auto b = r.section_bytes(name);
            if (name == "data") plain.assign(b.begin(), b.end()); else secs.emplace_back(name, std::string(reinterpret_cast<const char*>(b.data()), b.size()));
            if (name == "tensors") tj.assign(reinterpret_cast<const char*>(b.data()), b.size());
        }
    }
    std::array<std::uint8_t, 24> nonce{}; std::array<std::uint8_t, 16> mac{};
    if (forge::modelfile::os_entropy(nonce.data(), nonce.size()) != 0) raise(errc::io_error, "getentropy failed");
    auto ad = detail::enc_ad(tj); std::string ct(plain.size(), '\0');
    crypto_aead_lock(reinterpret_cast<std::uint8_t*>(ct.data()), mac.data(), key.data(), nonce.data(), reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(), plain.data(), plain.size());
    crypto_wipe(plain.data(), plain.size());
    secs.emplace_back("data", std::move(ct));
    secs.emplace_back("encryption", json::dump(json::object{{"alg", "xchacha20-poly1305"}, {"nonce", detail::hex(nonce.data(), 24)}, {"mac", detail::hex(mac.data(), 16)}, {"key_id", detail::enc_key_id(key)}}));
    detail::write_sections(out, std::move(secs));
}
inline model_key generate_model_key() { model_key k; if (forge::modelfile::os_entropy(k.data(), k.size()) != 0) raise(errc::io_error, "getentropy failed"); return k; }

} // namespace forge::modelfile
