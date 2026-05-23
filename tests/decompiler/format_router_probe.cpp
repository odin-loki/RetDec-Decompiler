/**
 * @file tests/decompiler/format_router_probe.cpp
 * @brief CLI probe for managed format detection (used by format_router_test.py).
 */

#include "managed_decompiler.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static std::vector<uint8_t> parseHex(const std::string& hex)
{
    std::vector<uint8_t> out;
    std::string token;
    std::istringstream iss(hex);
    while (iss >> token) {
        if (token.size() % 2 != 0)
            continue;
        for (std::size_t i = 0; i + 1 < token.size(); i += 2) {
            const auto byte = std::stoul(token.substr(i, 2), nullptr, 16);
            out.push_back(static_cast<uint8_t>(byte));
        }
    }
    return out;
}

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << "Usage: format_router_probe <hex bytes...>\n";
        return 2;
    }

    std::string combined;
    for (int i = 1; i < argc; ++i) {
        if (i > 1)
            combined += ' ';
        combined += argv[i];
    }

    const auto bytes = parseHex(combined);
    const auto fmt = detectManagedFormatFromBytes(bytes.data(), bytes.size());

    std::cout << static_cast<int>(fmt) << '\t' << managedFormatName(fmt) << '\t';
    if (const char* lang = managedOutputLangHint(fmt))
        std::cout << lang;
    std::cout << '\n';
    return 0;
}
