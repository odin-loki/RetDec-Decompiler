/**
 * @file src/gui/launch_options.cpp
 */

#include "retdec/gui/launch_options.h"

#include <QByteArray>
#include <QtGlobal>

#include <charconv>
#include <cstring>
#include <system_error>

namespace retdec::gui {
namespace {

bool envHeadless() {
    const QByteArray v = qgetenv("RETDEC_GUI_HEADLESS");
    if (v.isEmpty()) return false;
    return v == "1" || qstricmp(v.constData(), "true") == 0 || qstricmp(v.constData(), "yes") == 0
           || qstricmp(v.constData(), "on") == 0;
}

int parseIntOrZero(const char* s)
{
    if (!s || !s[0]) return 0;
    while (*s == ' ' || *s == '\t') ++s;
    int n = 0;
    const auto r = std::from_chars(s, s + std::strlen(s), n);
    if (r.ec != std::errc{}) return 0;
    return n;
}

} // namespace

ParsedLaunchOptions parseLaunchOptions(int argc, char** argv) {
    ParsedLaunchOptions out;
    out.headless       = envHeadless();
    out.headlessExitMs = 0;

    if (argc > 0 && argv && argv[0]) out.argStorage.emplace_back(argv[0]);

    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) continue;
        if (std::strcmp(argv[i], "--headless") == 0) {
            out.headless = true;
            continue;
        }
        if (std::strcmp(argv[i], "--headless-decompile") == 0) {
            out.headless           = true;
            out.headlessDecompile    = true;
            continue;
        }
        if (std::strcmp(argv[i], "--fast-decompile") == 0) {
            out.fastDecompile = true;
            continue;
        }
        if (std::strcmp(argv[i], "--headless-exit-ms") == 0 && i + 1 < argc) {
            out.headless = true;
            out.headlessExitMs = parseIntOrZero(argv[i + 1]);
            ++i;
            continue;
        }
        const char* a = argv[i];
        const char* k = "--headless-exit-ms=";
        const std::size_t klen = std::strlen(k);
        if (std::strncmp(a, k, klen) == 0) {
            out.headless = true;
            out.headlessExitMs = parseIntOrZero(a + klen);
            continue;
        }
        out.argStorage.emplace_back(argv[i]);
    }

    if (out.headless && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    out.argc = static_cast<int>(out.argStorage.size());
    out.argvPtrs.reserve(out.argStorage.size() + 1u);
    for (auto& s : out.argStorage) out.argvPtrs.push_back(s.data());
    out.argvPtrs.push_back(nullptr);

    return out;
}

} // namespace retdec::gui
