/**
 * @file fuzz_pdb.cpp
 * @brief libFuzzer harness for PDBFile::load_pdb_file.
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON ... && cmake --build build/linux --target fuzz_pdb
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/pdbparser/pdb_file.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static std::atomic<uint64_t> seq{0};
    const auto path = std::filesystem::temp_directory_path()
        / ("retdec-fuzz-pdb-" + std::to_string(seq++));

    {
        std::ofstream out(path, std::ios::binary);
        if (!out) {
            return 0;
        }
        out.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(size));
        if (!out) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            return 0;
        }
    }

    try {
        retdec::pdbparser::PDBFile pdb;
        if (pdb.load_pdb_file(path.string().c_str())
            == retdec::pdbparser::PDB_STATE_OK) {
            pdb.initialize(0);
        }
    } catch (...) {
        // Malformed PDB or allocation failure must not abort the process.
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}
