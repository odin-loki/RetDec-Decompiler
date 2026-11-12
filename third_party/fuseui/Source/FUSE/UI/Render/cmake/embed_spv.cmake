# Script mode: cmake -DSPV=<file.spv> -DOUT=<header> -DNAME=<symbol> -P embed_spv.cmake
file(READ "${SPV}" _hex HEX)
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1u," _words "${_hex}")
file(WRITE "${OUT}" "// Generated from ${SPV}. Do not edit.\n#pragma once\n#include <cstdint>\nalignas(4) static const std::uint32_t ${NAME}[] = {${_words}};\n")
