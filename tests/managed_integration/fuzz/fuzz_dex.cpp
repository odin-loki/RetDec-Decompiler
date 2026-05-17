/**
 * @file fuzz_dex.cpp
 * @brief libFuzzer harness for the Android DEX file parser.
 *
 * Build:
 *   clang++ -std=c++17 -fsanitize=fuzzer,address \
 *     -I<repo>/include \
 *     fuzz_dex.cpp \
 *     -L<build>/src/dex_parser -lretdec-dex-parser \
 *     -L<build>/src/bc_module  -lretdec-bc-module \
 *     -o fuzz_dex
 *
 * Run:
 *   mkdir -p corpus_dex && cp <fixtures>/dex/*.dex corpus_dex/
 *   ./fuzz_dex corpus_dex/ -max_total_time=600 -jobs=4 -runs=1000000
 *
 * @copyright (c) 2024 Odin Loch Trading as Imortek
 */

#include "retdec/dex_parser/dex_header.h"
#include "retdec/dex_parser/dex_class_parser.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    using namespace retdec::dex_parser;

    try {
        DexFile dex = DexFile::parse(data, size);

        // If header parsed successfully, also try parsing all class defs.
        DexParseOptions opts;
        opts.parseBytecode    = true;
        opts.parseAnnotations = true;
        opts.resolveGenerics  = true;
        opts.strict           = false;

        DexClassParser parser(dex, opts);
        for (uint32_t i = 0; i < dex.classDefsSize(); ++i) {
            (void)parser.parseClass(i);
        }
    } catch (const std::exception&) {
        // Expected for malformed input — must not abort/crash.
    } catch (...) {
        // Any other exception is also acceptable; only hard crashes are bugs.
    }

    return 0;
}
