/**
 * @file src/gui/launch_options.cpp
 */

#include "retdec/gui/launch_options.h"

#include <QByteArray>
#include <QtGlobal>

#include <cstdlib>
#include <cstring>

namespace retdec::gui {
namespace {

bool envHeadless() {
    const QByteArray v = qgetenv("RETDEC_GUI_HEADLESS");
    if (v.isEmpty()) return false;
    return v == "1" || qstricmp(v.constData(), "true") == 0 || qstricmp(v.constData(), "yes") == 0
           || qstricmp(v.constData(), "on") == 0;
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
            out.headlessExitMs = std::atoi(argv[i + 1]);
            ++i;
            continue;
        }
        const char* a = argv[i];
        const char* k = "--headless-exit-ms=";
        const std::size_t klen = std::strlen(k);
        if (std::strncmp(a, k, klen) == 0) {
            out.headless = true;
            out.headlessExitMs = std::atoi(a + klen);
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
