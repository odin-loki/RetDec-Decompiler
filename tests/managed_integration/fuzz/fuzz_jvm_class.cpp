/**
 * @file fuzz_jvm_class.cpp
 * @brief libFuzzer harness for the JVM .class parser.
 *
 * Build:
 *   clang++ -std=c++17 -fsanitize=fuzzer,address \
 *     -I<repo>/include \
 *     fuzz_jvm_class.cpp \
 *     -L<build>/src/jvm_parser -lretdec-jvm-parser \
 *     -L<build>/src/bc_module  -lretdec-bc-module \
 *     -o fuzz_jvm_class
 *
 * Run:
 *   mkdir -p corpus_jvm && cp <fixtures>/java/*.class corpus_jvm/
 *   ./fuzz_jvm_class corpus_jvm/ -max_total_time=600 -jobs=4 -runs=1000000
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/jvm_parser/jvm_class_parser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace retdec::jvm_parser;

    // Try both strict and lenient parsing modes.
    JvmParseOptions lenient;
    lenient.strictVersion = false;

    JvmParseOptions strict;
    strict.strictVersion = true;

    // These must not crash regardless of input.
    (void)parseClassFile(data, size, lenient);
    (void)parseClassFile(data, size, strict);

    return 0;
}
