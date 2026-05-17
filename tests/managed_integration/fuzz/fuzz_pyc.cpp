/**
 * @file fuzz_pyc.cpp
 * @brief libFuzzer harness for the Python .pyc parser.
 *
 * Build:
 *   clang++ -std=c++17 -fsanitize=fuzzer,address \
 *     -I<repo>/include \
 *     fuzz_pyc.cpp \
 *     -L<build>/src/pyc_parser -lretdec-pyc-parser \
 *     -L<build>/src/bc_module  -lretdec-bc-module \
 *     -o fuzz_pyc
 *
 * Run:
 *   mkdir -p corpus_pyc && find <fixtures>/python -name '*.pyc' | xargs cp -t corpus_pyc/
 *   ./fuzz_pyc corpus_pyc/ -max_total_time=600 -jobs=4 -runs=1000000
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/pyc_parser/pyc_reader.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    retdec::pyc_parser::PycReader reader;
    // Must not abort/crash regardless of input.
    (void)reader.read(data, size, "fuzz_input.pyc");
    return 0;
}
