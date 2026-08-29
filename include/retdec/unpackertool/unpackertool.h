/**
 * \file include/retdec/unpackertool/unpackertool.h
 * \brief Unpackertool library.
 * \copyright (c) 2017 Avast Software, licensed under the MIT license
 * \copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#ifndef RETDEC_UNPACKERTOOL_UNPACKERTOOL_H
#define RETDEC_UNPACKERTOOL_UNPACKERTOOL_H

#include <string>

#include "retdec/utils/string.h"

namespace retdec {
namespace unpackertool {

int _main(int argc, char** argv);

/// @c cpdetect heuristic packer names — no UPX/MPRESS static unpacker exists.
inline bool isHeuristicOnlyPackerLabel(const std::string &name)
{
	return retdec::utils::startsWith(name, "Unknown packer (");
}

} // namespace unpackertool
} // namespace retdec

#endif
