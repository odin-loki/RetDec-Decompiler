/**
 * @file fuzz_apk.cpp
 * @brief libFuzzer harness for ApkReader (APK/ZIP + embedded DEX).
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_apk
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/dex_parser/dex_apk_reader.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        retdec::dex_parser::ApkReader reader;
        (void)reader.readApk(data, size);
    } catch (const std::exception&) {
        // Malformed ZIP/DEX is expected; hard crashes are bugs.
    } catch (...) {
    }
    return 0;
}
