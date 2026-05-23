/**
 * @file plugin_load_probe.cpp
 * @brief Minimal shared library that logs when loaded (LLVM PluginLoader probe).
 *
 * Build with examples/decompiler_plugin/CMakeLists.txt. Not linked into RetDec.
 */

#include <cstdio>

#if defined(_WIN32)
#  define RETDEC_PLUGIN_EXPORT __declspec(dllexport)
#else
#  define RETDEC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace
{

struct LoadProbe
{
    LoadProbe()
    {
        std::fprintf(stderr, "[retdec-plugin-probe] shared library loaded\n");
        std::fflush(stderr);
    }
};

LoadProbe g_probe;

} // namespace

extern "C" RETDEC_PLUGIN_EXPORT const char* retdec_plugin_probe_version()
{
    return "1.0.0";
}
