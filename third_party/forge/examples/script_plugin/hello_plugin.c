/* Example FORGE script plugin. Build:  cc -shared -fPIC -I script/include hello_plugin.c -o libhello.so
 * Use:   import "libhello.so" as hello;  std::println("{}", hello.add(2, 3));  */
#include <forge/script/plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const forge_plugin_api* host;

static char* dup_str(const char* s) { size_t n = strlen(s) + 1; char* o = host->alloc(n); memcpy(o, s, n); return o; }

/* add(a, b): args arrive as a JSON array of two numbers. */
static int add(const char* args, char** out, void* user) {
    (void)user; double a = 0, b = 0;
    if (sscanf(args, "[%lf,%lf]", &a, &b) != 2) { *out = dup_str("add expects two numbers"); return 1; }
    char buf[64]; snprintf(buf, sizeof buf, "%.17g", a + b); *out = dup_str(buf); return 0;
}

/* greet(name): returns a JSON string; the argument is passed through as JSON. */
static int greet(const char* args, char** out, void* user) {
    const char* prefix = (const char*)user; size_t n = strlen(args);
    if (n < 4 || args[1] != '"') { *out = dup_str("greet expects a string"); return 2; }
    char* o = host->alloc(n + strlen(prefix) + 8);
    sprintf(o, "\"%s%.*s\"", prefix, (int)(n - 4), args + 2);
    *out = o; return 0;
}

static int fail(const char* args, char** out, void* user) { (void)args; (void)user; *out = dup_str("this always fails"); return 7; }

FORGE_PLUGIN_EXPORT int forge_script_plugin_init(const forge_plugin_api* api) {
    if (api->abi != FORGE_SCRIPT_PLUGIN_ABI) return 1;
    host = api;
    api->def(api->reg, "add", 2, 2, add, NULL);
    api->def(api->reg, "greet", 1, 1, greet, (void*)"hello, ");
    api->def(api->reg, "fail", 0, -1, fail, NULL);
    api->constant(api->reg, "VERSION", "\"1.0\"");
    return 0;
}
