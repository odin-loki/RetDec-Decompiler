/**
 * @file fuzz_elf.cpp
 * @brief libFuzzer harness for the ELF parser (retdec::fileformat::ElfFormat).
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_elf
 *
 * Run:
 *   ./fuzz_elf corpus_elf/ -max_total_time=600 -jobs=4 -runs=1000000
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/fileformat/file_format/elf/elf_format.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    retdec::fileformat::ElfFormat parser(data, size);
    // Must not abort/crash regardless of input; error returns are acceptable.
    (void)parser.isInValidState();
    return 0;
}
