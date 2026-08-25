/**
 * @file fuzz_unpacker_plugins.cpp
 * @brief libFuzzer harness for UPX and MPRESS unpacker plugins.
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON -DRETDEC_ENABLE_UNPACKERTOOL=ON ...
 *   cmake --build build/linux --target fuzz_unpacker_plugins
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "unpackertool/plugins/mpress/mpress.h"
#include "unpackertool/plugins/upx/upx.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::atomic<uint64_t> seq{0};
    const auto id = seq++;
    const auto inPath = std::filesystem::temp_directory_path()
        / ("retdec-fuzz-upk-in-" + std::to_string(id));
    const auto outPath = std::filesystem::temp_directory_path()
        / ("retdec-fuzz-upk-out-" + std::to_string(id));

    {
        std::ofstream out(inPath, std::ios::binary);
        if (!out) {
            return 0;
        }
        out.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(inPath, ec);
            return 0;
        }
    }

    retdec::unpackertool::Plugin::Arguments args;
    args.inputFile = inPath.string();
    args.outputFile = outPath.string();
    args.brute = false;

    try {
        if ((size == 0 ? 0 : data[0]) % 2 == 0) {
            retdec::unpackertool::upx::UpxPlugin plugin;
            (void)plugin.run(args);
        } else {
            retdec::unpackertool::mpress::MpressPlugin plugin;
            (void)plugin.run(args);
        }
    } catch (...) {
        // Malformed packed input is expected; hard crashes are bugs.
    }

    std::error_code ec;
    std::filesystem::remove(inPath, ec);
    std::filesystem::remove(outPath, ec);
    return 0;
}
