/* A CPU stand-in for libze_loader.so.1, used only by tests (built against the vendored ze_api.h, so struct
   layouts match the real loader). zeModuleCreate accepts the OpenCL C source (FORGE_ZE_TEST_IMAGE=opencl-c)
   and kernels run from kernels.cl compiled here as C. MOCK_DEVICES (env) sets the device count. */
#include <level-zero/ze_api.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define MOCK_ALIGNED_ALLOC(a, n) _aligned_malloc((n), (a))   /* no C11 aligned_alloc in the MS CRT */
#define MOCK_ALIGNED_FREE(p) _aligned_free(p)
#else
#define MOCK_ALIGNED_ALLOC(a, n) aligned_alloc((a), (n))
#define MOCK_ALIGNED_FREE(p) free(p)
#endif

#define FORGE_CL_HOST_SHIM
#define FORGE_K static
#define G
#ifdef _MSC_VER
static __declspec(thread) size_t mock_gid;
#else
static __thread size_t mock_gid;
#endif
static size_t get_global_id(int d) { (void)d; return mock_gid; }
static unsigned as_uint(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
#include "kernels.cl"
#define MOCK_MEM(p) (*(char**)(p))
#include "kernels_cl_mock.inc"

typedef struct { int idx; uint32_t gx; void* args[16]; char store[16][8]; } kern;
static int ndev(void) { const char* e = getenv("MOCK_DEVICES"); return e ? atoi(e) : 1; }
static size_t g_live;
ze_result_t zeInit(ze_init_flags_t f) { (void)f; return ZE_RESULT_SUCCESS; }
ze_result_t zeDriverGet(uint32_t* n, ze_driver_handle_t* d) { if (d && *n) d[0] = (ze_driver_handle_t)1; *n = 1; return ZE_RESULT_SUCCESS; }
ze_result_t zeDeviceGet(ze_driver_handle_t dr, uint32_t* n, ze_device_handle_t* d) {
    (void)dr; uint32_t c = (uint32_t)ndev();
    if (d) for (uint32_t i = 0; i < *n && i < c; ++i) d[i] = (ze_device_handle_t)(uintptr_t)(i + 1);
    *n = c; return ZE_RESULT_SUCCESS;
}
ze_result_t zeDeviceGetProperties(ze_device_handle_t d, ze_device_properties_t* p) {
    if (p->stype != ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES) return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    snprintf(p->name, sizeof p->name, "forge mock level zero %d", (int)(uintptr_t)d - 1); return ZE_RESULT_SUCCESS;
}
ze_result_t zeDeviceGetMemoryProperties(ze_device_handle_t d, uint32_t* n, ze_device_memory_properties_t* p) {
    (void)d; if (p && *n) p[0].totalSize = (uint64_t)1 << 30; *n = 1; return ZE_RESULT_SUCCESS; }
ze_result_t zeContextCreate(ze_driver_handle_t d, const ze_context_desc_t* desc, ze_context_handle_t* c) {
    (void)d; if (desc->stype != ZE_STRUCTURE_TYPE_CONTEXT_DESC) return ZE_RESULT_ERROR_INVALID_ARGUMENT; *c = malloc(1); return ZE_RESULT_SUCCESS; }
ze_result_t zeContextDestroy(ze_context_handle_t c) { free(c); return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListCreateImmediate(ze_context_handle_t c, ze_device_handle_t d, const ze_command_queue_desc_t* q, ze_command_list_handle_t* l) {
    (void)c; (void)d; if (q->stype != ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC) return ZE_RESULT_ERROR_INVALID_ARGUMENT; *l = malloc(1); return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListDestroy(ze_command_list_handle_t l) { free(l); return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListHostSynchronize(ze_command_list_handle_t l, uint64_t t) { (void)l; (void)t; return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListAppendMemoryCopy(ze_command_list_handle_t l, void* d, const void* s, size_t n, ze_event_handle_t e, uint32_t ne, ze_event_handle_t* w) {
    (void)l; (void)e; (void)ne; (void)w; memmove(d, s, n); return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListAppendBarrier(ze_command_list_handle_t l, ze_event_handle_t e, uint32_t ne, ze_event_handle_t* w) { (void)l; (void)e; (void)ne; (void)w; return ZE_RESULT_SUCCESS; }
ze_result_t zeMemAllocDevice(ze_context_handle_t c, const ze_device_mem_alloc_desc_t* d, size_t n, size_t al, ze_device_handle_t dev, void** p) {
    (void)c; (void)dev; if (d->stype != ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC) return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    size_t a2 = al < 16 ? 16 : al; size_t* b = MOCK_ALIGNED_ALLOC(a2, ((n + 16 + a2 - 1) / a2) * a2 + a2); if (!b) return ZE_RESULT_ERROR_OUT_OF_DEVICE_MEMORY;
    g_live += n; *p = b; return ZE_RESULT_SUCCESS; }
ze_result_t zeMemFree(ze_context_handle_t c, void* p) { (void)c; MOCK_ALIGNED_FREE(p); return ZE_RESULT_SUCCESS; }
ze_result_t zeModuleCreate(ze_context_handle_t c, ze_device_handle_t d, const ze_module_desc_t* m, ze_module_handle_t* mod, ze_module_build_log_handle_t* log) {
    (void)c; (void)d; if (log) *log = NULL;
    if (m->stype != ZE_STRUCTURE_TYPE_MODULE_DESC || m->format != ZE_MODULE_FORMAT_IL_SPIRV) return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    if (m->inputSize < 100 || !strstr((const char*)m->pInputModule, "forge_attention")) return ZE_RESULT_ERROR_MODULE_BUILD_FAILURE;
    *mod = malloc(1); return ZE_RESULT_SUCCESS; }
ze_result_t zeModuleDestroy(ze_module_handle_t m) { free(m); return ZE_RESULT_SUCCESS; }
ze_result_t zeModuleBuildLogGetString(ze_module_build_log_handle_t l, size_t* n, char* s) { (void)l; (void)s; *n = 0; return ZE_RESULT_SUCCESS; }
ze_result_t zeModuleBuildLogDestroy(ze_module_build_log_handle_t l) { (void)l; return ZE_RESULT_SUCCESS; }
ze_result_t zeKernelCreate(ze_module_handle_t m, const ze_kernel_desc_t* d, ze_kernel_handle_t* k) {
    (void)m; if (d->stype != ZE_STRUCTURE_TYPE_KERNEL_DESC) return ZE_RESULT_ERROR_INVALID_ARGUMENT;
    for (int i = 0; forge_tramps[i].name; ++i) if (!strcmp(forge_tramps[i].name, d->pKernelName)) { kern* x = calloc(1, sizeof *x); x->idx = i; *k = (ze_kernel_handle_t)x; return ZE_RESULT_SUCCESS; }
    return ZE_RESULT_ERROR_INVALID_KERNEL_NAME; }
ze_result_t zeKernelDestroy(ze_kernel_handle_t k) { free(k); return ZE_RESULT_SUCCESS; }
ze_result_t zeKernelSetGroupSize(ze_kernel_handle_t k, uint32_t x, uint32_t y, uint32_t z) { (void)y; (void)z; ((kern*)k)->gx = x; return ZE_RESULT_SUCCESS; }
ze_result_t zeKernelSetArgumentValue(ze_kernel_handle_t k_, uint32_t i, size_t n, const void* v) {
    kern* k = (kern*)k_; if (i >= 16 || n > 8) return ZE_RESULT_ERROR_INVALID_KERNEL_ARGUMENT_INDEX;
    memcpy(k->store[i], v, n); k->args[i] = k->store[i]; return ZE_RESULT_SUCCESS; }
ze_result_t zeCommandListAppendLaunchKernel(ze_command_list_handle_t l, ze_kernel_handle_t k_, const ze_group_count_t* g, ze_event_handle_t e, uint32_t ne, ze_event_handle_t* w) {
    (void)l; (void)e; (void)ne; (void)w; kern* k = (kern*)k_;
    size_t total = (size_t)g->groupCountX * k->gx;
    for (size_t i = 0; i < total; ++i) { mock_gid = i; forge_tramps[k->idx].f(k->args); }
    return ZE_RESULT_SUCCESS; }
