/* FORGE script native plugin ABI (C, stable). A plugin is a shared library (.so / .dylib / .dll) that
 * exports `forge_script_plugin_init`. Scripts load it with `import "libname.so" as m;`,
 * `#include "libname.so"` or `forge::plugin(path)`, then call `m.fn(args...)`.
 *
 * Values cross the boundary as JSON text (UTF-8): the arguments as a JSON array, the result as any
 * JSON value. Errors: return non-zero and put a message in *out; the script sees PluginError.
 * Strings written to *out must be allocated with api->alloc (the host frees them). The api pointer
 * stays valid while the plugin is loaded; def() and constant() only work inside forge_script_plugin_init.
 * Calls may arrive from several script tasks at once; make functions thread-safe or say so.
 * Plugins run native code with the user's privileges, so loading one is refused in sandboxed runs. */
#ifndef FORGE_SCRIPT_PLUGIN_H
#define FORGE_SCRIPT_PLUGIN_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FORGE_SCRIPT_PLUGIN_ABI 1

typedef int (*forge_plugin_fn)(const char* args_json, char** out, void* user);

typedef struct forge_plugin_api {
    int abi;                                                    /* FORGE_SCRIPT_PLUGIN_ABI of the host */
    void* reg;                                                  /* opaque; pass back to def() */
    /* Register `name`, callable with min..max arguments (max < 0: any number). */
    void (*def)(void* reg, const char* name, int min_args, int max_args, forge_plugin_fn fn, void* user);
    /* Constant exposed as a field (value given as JSON). */
    void (*constant)(void* reg, const char* name, const char* json);
    char* (*alloc)(size_t n);                                   /* host allocator for *out */
} forge_plugin_api;

#if defined(_WIN32)
#define FORGE_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FORGE_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

/* Implemented by the plugin. Return 0 on success; non-zero refuses the load (e.g. abi mismatch). */
FORGE_PLUGIN_EXPORT int forge_script_plugin_init(const forge_plugin_api* api);

#ifdef __cplusplus
}
#endif
#endif
