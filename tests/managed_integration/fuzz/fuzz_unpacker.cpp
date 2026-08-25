/**
 * @file fuzz_unpacker.cpp
 * @brief libFuzzer harness for unpacker NRV/LZMA/LZMAT decompressors.
 *
 * Build:
 *   cmake -DRETDEC_FUZZ=ON -DRETDEC_ENABLE_UNPACKER=ON ...
 *   cmake --build build/linux --target fuzz_unpacker
 *
 * @copyright (c) 2026 Odin Loch Trading as Imortek
 */

#include "retdec/unpacker/decompression/lzma/lzma_data.h"
#include "retdec/unpacker/decompression/lzmat/lzmat_data.h"
#include "retdec/unpacker/decompression/nrv/nrv2b_data.h"
#include "retdec/unpacker/decompression/nrv/nrv2d_data.h"
#include "retdec/unpacker/decompression/nrv/nrv2e_data.h"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) {
        return 0;
    }

    const std::vector<uint8_t> payload(data + 1, data + size);
    retdec::utils::DynamicBuffer input(payload);
    retdec::utils::DynamicBuffer output(65536u);

    try {
        switch (data[0] % 5) {
        case 0: {
            retdec::unpacker::BitParser8 parser;
            retdec::unpacker::Nrv2bData nrv(input, &parser);
            (void)nrv.decompress(output);
            break;
        }
        case 1: {
            retdec::unpacker::BitParserLe32 parser;
            retdec::unpacker::Nrv2dData nrv(input, &parser);
            (void)nrv.decompress(output);
            break;
        }
        case 2: {
            retdec::unpacker::BitParser8 parser;
            retdec::unpacker::Nrv2eData nrv(input, &parser);
            (void)nrv.decompress(output);
            break;
        }
        case 3: {
            const uint8_t pb = static_cast<uint8_t>(data[1] % 5);
            const uint8_t lp = static_cast<uint8_t>((size > 2 ? data[2] : 0) % 5);
            const uint8_t lc = static_cast<uint8_t>((size > 3 ? data[3] : 0) % 9);
            retdec::unpacker::LzmaData lzma(input, pb, lp, lc);
            (void)lzma.decompress(output);
            break;
        }
        default: {
            retdec::unpacker::LzmatData lzmat(input);
            (void)lzmat.decompress(output);
            break;
        }
        }
    } catch (const std::exception&) {
    } catch (...) {
    }
    return 0;
}
