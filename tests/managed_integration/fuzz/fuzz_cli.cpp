/**
 * @file fuzz_cli.cpp
 * @brief libFuzzer harness for CLIReader (.NET PE / CIL metadata).
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_cli
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/cli_parser/cli_reader.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    try {
        retdec::cli_parser::CLIReader reader;
        (void)reader.read(data, size, "fuzz");
    } catch (const std::exception&) {
        // Malformed PE/CLI is expected; hard crashes are bugs.
    } catch (...) {
    }
    return 0;
}
