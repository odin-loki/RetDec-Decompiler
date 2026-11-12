/*
 * tests/gui/data/test_plugin.c
 *
 * A retdec-gui plugin written against the C ABI in
 * include/retdec/gui/model/plugins.h. settings_test.cpp compiles it with the
 * system C compiler at run time (cc -shared -fPIC) and loads it through
 * PluginManager::loadPlugin(), so the real dynlib path is exercised.
 */

#include "retdec/gui/model/plugins.h"

#include <stdlib.h>
#include <string.h>

static int g_initialized = 0;

static char* dup_concat(const char* a, const char* b)
{
	size_t la = strlen(a), lb = strlen(b);
	char* out = (char*) malloc(la + lb + 1);
	if (!out) return NULL;
	memcpy(out, a, la);
	memcpy(out + la, b, lb + 1);
	return out;
}

static int test_initialize(void)
{
	g_initialized = 1;
	return 1;
}

static void test_shutdown(void)
{
	g_initialized = 0;
}

static void test_free(char* s)
{
	free(s);
}

static char* test_transform(const char* c, size_t length)
{
	(void) length;
	return dup_concat("// TRANSFORMED BY C PLUGIN\n", c);
}

static char* test_summary(void)
{
	return dup_concat(g_initialized ? "initialized" : "not initialized", "");
}

static char* test_analyse(const RetdecGuiPipelineContext* ctx)
{
	return dup_concat(ctx->decompiled_text, "\n// analysed by C plugin\n");
}

static const RetdecGuiPluginV1 kPlugin = {
	.struct_size = sizeof(RetdecGuiPluginV1),
	.abi_version = RETDEC_GUI_PLUGIN_ABI_VERSION,
	.id = "com.retdec.test.cplugin",
	.name = "C test plugin",
	.version = "1.2",
	.description = "Fixture for settings_test",
	.author = "retdec tests",
	.dependencies = NULL,
	.kinds = RETDEC_GUI_PLUGIN_OUTPUT | RETDEC_GUI_PLUGIN_ANALYSIS,
	.initialize = test_initialize,
	.shutdown = test_shutdown,
	.free_string = test_free,
	.format_name = "C Test Format",
	.file_extension = ".ctest",
	.transform = test_transform,
	.analyse = test_analyse,
	.summary = test_summary,
};

RETDEC_GUI_PLUGIN_EXPORT const RetdecGuiPluginV1* retdec_gui_plugin_v1(void)
{
	return &kPlugin;
}
