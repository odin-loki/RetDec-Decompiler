#include "retdec/neural/model_verify.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace retdec::neural {

namespace {

std::string sha256HexOfFile(const std::string& path)
{
    std::string cmd = "sha256sum \"" + path + "\"";
#if defined(_WIN32)
    cmd = "certutil -hashfile \"" + path + "\" SHA256";
#endif
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return {};

    char buffer[256];
    std::string output;
    while (fgets(buffer, sizeof(buffer), pipe))
        output += buffer;
    pclose(pipe);

#if defined(_WIN32)
  // certutil: take hex line after header
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() >= 64) {
            std::string hex;
            for (char c : line) {
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
                    hex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (hex.size() >= 64) return hex.substr(0, 64);
        }
    }
    return {};
#else
    std::istringstream iss(output);
    std::string hex;
    iss >> hex;
    return hex;
#endif
}

} // namespace

bool verifyModelSha256(const std::string& modelPath)
{
    const char* expected = std::getenv("RETDEC_NEURAL_MODEL_SHA256");
    if (!expected || !expected[0]) return true;

    const std::string actual = sha256HexOfFile(modelPath);
    if (actual.empty()) return false;

    std::string exp(expected);
    for (char& c : exp)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return actual == exp;
}

} // namespace retdec::neural
