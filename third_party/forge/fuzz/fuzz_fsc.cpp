// .fsc cache loader: a cache file is untrusted input. The input is fed (1) whole to fsc::decode, keyed with
// its own header key so the checksum and signature paths run, and (2) with the header and trailer removed
// straight to the payload decoder, which the checksum would otherwise shield from mutations. Rejection is
// fine; a crash, a sanitizer report, an exception escaping, or a decoded tree that does not survive
// re-encoding is a finding. Seeds: fuzz/corpus/fsc (tests/test_script_fsc.cpp writes them, see there).
#include "fuzz.hpp"
#include "fsc.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fsc = forge::script::fsc;

static void check_roundtrip(const std::shared_ptr<const forge::script::FnDecl>& f) {
    fsc::key_t k{};
    auto a = fsc::encode(*f, k);
    auto back = fsc::decode(std::span<const std::uint8_t>(a), k, {});
    if (!back) { std::fprintf(stderr, "decoded tree does not re-decode\n"); std::abort(); }
    if (fsc::encode(**back, k) != a) { std::fprintf(stderr, "re-encoding is not stable\n"); std::abort(); }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::span<const std::uint8_t> in(data, size);
    fsc::key_t key{};
    if (size >= 40) std::memcpy(key.data(), data + 8, 32);
    fsc::trust t;
    if (size >= 96) { std::array<std::uint8_t, 32> pub{}; std::memcpy(pub.data(), data + size - 96, 32); t.pubkeys.push_back(pub); }
    if (auto f = fsc::decode(in, key, t)) check_roundtrip(*f);
    t.require_signed = true;
    (void)fsc::decode(in, key, t);
    if (size > 48 + 33) {
        if (auto f = fsc::decode_payload(in.subspan(48, size - 48 - 33))) check_roundtrip(f);
    }
    if (auto f = fsc::decode_payload(in)) check_roundtrip(f);
    return 0;
}
