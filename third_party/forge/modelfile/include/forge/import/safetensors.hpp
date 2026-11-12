#pragma once
#include <forge/core/mapped.hpp>
// safetensors reader (mmap): 8-byte LE header length, JSON header, raw little-endian tensor bytes.
#include <forge/core/dtypes.hpp>
#include <forge/core/tensor.hpp>
#include <forge/json/json.hpp>
#include <forge/security/enforce.hpp>
#include <cstring>
#include <map>

namespace forge::import {

class safetensors {
public:
    struct entry { std::string dtype; shape_t shape; std::uint64_t begin = 0, end = 0; };
    explicit safetensors(const std::string& path) : path_(path) {
        security::enforce(security::capability::fs_read, path);   // FSS CAP-1 under an active policy (no-op otherwise)
        map_ = forge::map_readonly(path, size_);
        try {
            if (!map_) raise(errc::format_error, "{}: empty file", path);
            if (size_ < 8) raise(errc::format_error, "{}: bad safetensors header", path);
            std::uint64_t hl; std::memcpy(&hl, map_, 8);
            // hl > size_ - 8, not 8 + hl > size_: that overflows for hl near 2^64 (fuzz finding, FreeBSD libc++ trap)
            if (hl > size_ - 8) raise(errc::format_error, "{}: bad safetensors header", path);
            auto h = json::parse({reinterpret_cast<const char*>(map_ + 8), hl});
            base_ = 8 + hl;
            for (auto& [k, v] : h.obj()) {
                if (k == "__metadata__") continue;
                entry e; e.dtype = v["dtype"].str();
                for (auto& d : v["shape"].arr()) e.shape.push_back(d.i());
                const auto& off = v["data_offsets"].arr();   // exactly [begin, end]; [n] threw std::out_of_range (fuzz, libc++)
                if (off.size() != 2) raise(errc::format_error, "{}: tensor {} needs 2 data_offsets", path, k);
                e.begin = std::uint64_t(off[0].num()); e.end = std::uint64_t(off[1].num());
                if (e.begin > e.end || e.end > size_ - base_) raise(errc::format_error, "{}: tensor {} out of bounds", path, k);
                entries_[k] = e;
            }
        } catch (...) { forge::unmap_readonly(map_, size_); map_ = nullptr; throw; }   // no dtor on a throwing ctor
    }
    safetensors(const safetensors&) = delete;
    ~safetensors() { forge::unmap_readonly(map_, size_); }
    const std::map<std::string, entry>& entries() const { return entries_; }
    bool has(const std::string& n) const { return entries_.count(n) != 0; }
    tensor load(const std::string& n) const {
        auto it = entries_.find(n);
        if (it == entries_.end()) raise(errc::not_found, "{}: no tensor {}", path_, n);
        const auto& e = it->second;
        tensor t(e.shape.empty() ? shape_t{1} : e.shape);
        const auto* p = map_ + base_ + e.begin;
        std::size_t n_el = t.size();
        auto need = [&](std::size_t b) { if (e.end - e.begin != n_el * b) raise(errc::format_error, "{}: size mismatch", n); };
        if (e.dtype == "F32") { need(4); std::memcpy(t.data.data(), p, n_el * 4); }
        else if (e.dtype == "F16") { need(2); for (std::size_t i = 0; i < n_el; ++i) { u16 b; std::memcpy(&b, p + 2 * i, 2); t.data[i] = float(f16::from_bits(b)); } }
        else if (e.dtype == "BF16") { need(2); for (std::size_t i = 0; i < n_el; ++i) { u16 b; std::memcpy(&b, p + 2 * i, 2); t.data[i] = float(bf16::from_bits(b)); } }
        else if (e.dtype == "F64") { need(8); for (std::size_t i = 0; i < n_el; ++i) { double d; std::memcpy(&d, p + 8 * i, 8); t.data[i] = float(d); } }
        else raise(errc::unsupported, "{}: dtype {} not supported", n, e.dtype);
        return t;
    }
private:
    std::string path_;
    const std::uint8_t* map_ = nullptr;
    std::size_t size_ = 0, base_ = 0;
    std::map<std::string, entry> entries_;
};

} // namespace forge::import
