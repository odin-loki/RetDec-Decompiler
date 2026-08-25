/**
 * @file fuzz_jar.cpp
 * @brief libFuzzer harness for JarReader (JAR/WAR/EAR ZIP).
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_jar
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/jvm_parser/jvm_jar_reader.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        retdec::jvm_parser::JarReader reader;
        (void)reader.read(data, size);
    } catch (const std::exception&) {
        // Malformed ZIP/class is expected; hard crashes are bugs.
    } catch (...) {
    }
    return 0;
}
