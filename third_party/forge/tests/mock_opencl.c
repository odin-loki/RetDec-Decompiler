/* A CPU stand-in for libOpenCL.so.1, used only by tests. It implements the entry points grid/opencl.hpp calls
   and runs each kernel from the generated kernels.cl itself, compiled here as C through a small shim, so the
   OpenCL C bodies (not a hand copy) are what gets checked. MOCK_DEVICES (env) sets the device count. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef struct { size_t size; char* data; int sub; } mem;
#define MOCK_MEM(p) (((mem*)*(void**)(p))->data)
#include "kernels_cl_mock.inc"

typedef struct { int idx; void* args[16]; char store[16][8]; } kern;
static int ndev(void) { const char* e = getenv("MOCK_DEVICES"); return e ? atoi(e) : 1; }
int clGetPlatformIDs(unsigned n, void** p, unsigned* np) { if (np) *np = 1; if (n && p) p[0] = (void*)1; return 0; }
int clGetDeviceIDs(void* p, uint64_t type, unsigned n, void** d, unsigned* nd) {
    (void)p; int c = (type & 4) ? ndev() : 0;   /* GPUs only */
    if (nd) *nd = (unsigned)c;
    for (unsigned i = 0; d && i < n && (int)i < c; ++i) d[i] = (void*)(uintptr_t)(i + 1);
    return c ? 0 : -1;   /* CL_DEVICE_NOT_FOUND */
}
int clGetDeviceInfo(void* d, unsigned what, size_t n, void* v, size_t* got) {
    (void)got;
    if (what == 0x102B) snprintf((char*)v, n, "forge mock opencl %d", (int)(uintptr_t)d - 1);
    else if (what == 0x1019) *(unsigned*)v = 1024;
    else if (what == 0x101F) *(uint64_t*)v = (uint64_t)1 << 30;
    else return -30;
    return 0;
}
void* clCreateContext(const intptr_t* p, unsigned n, void* const* d, void* cb, void* u, int* e) { (void)p; (void)n; (void)d; (void)cb; (void)u; *e = 0; return malloc(1); }
void* clCreateCommandQueue(void* c, void* d, uint64_t f, int* e) { (void)c; (void)d; (void)f; *e = 0; return malloc(1); }
void* clCreateProgramWithSource(void* c, unsigned n, const char** s, const size_t* l, int* e) {
    (void)c; (void)n; *e = strstr(s[0], "forge_attention") && l[0] > 100 ? 0 : -30; return malloc(1); }
int clBuildProgram(void* p, unsigned n, void* const* d, const char* o, void* cb, void* u) { (void)p; (void)n; (void)d; (void)o; (void)cb; (void)u; return 0; }
int clGetProgramBuildInfo(void* p, void* d, unsigned w, size_t n, void* v, size_t* g) { (void)p; (void)d; (void)w; (void)n; (void)v; if (g) *g = 0; return 0; }
void* clCreateKernel(void* p, const char* name, int* e) {
    (void)p;
    for (int i = 0; forge_tramps[i].name; ++i) if (!strcmp(forge_tramps[i].name, name)) { kern* k = calloc(1, sizeof *k); k->idx = i; *e = 0; return k; }
    *e = -46; return NULL;
}
int clSetKernelArg(void* k_, unsigned i, size_t n, const void* v) {
    kern* k = k_; if (i >= 16 || n > 8) return -51;
    memcpy(k->store[i], v, n); k->args[i] = k->store[i]; return 0;
}
int clEnqueueNDRangeKernel(void* q, void* k_, unsigned dim, const size_t* off, const size_t* g, const size_t* l, unsigned ne, const void* ev, void* oe) {
    (void)q; (void)dim; (void)off; (void)ne; (void)ev; (void)oe;
    kern* k = k_; if (g[0] % l[0]) return -54;
    for (size_t i = 0; i < g[0]; ++i) { mock_gid = i; forge_tramps[k->idx].f(k->args); }
    return 0;
}
void* clCreateBuffer(void* c, uint64_t f, size_t n, void* host, int* e) {
    (void)c; mem* m = calloc(1, sizeof *m); m->size = n; m->data = malloc(n);
    if ((f & 0x20) && host) memcpy(m->data, host, n);
    *e = 0; return m;
}
void* clCreateSubBuffer(void* b, uint64_t f, unsigned t, const void* info, int* e) {
    (void)f; mem* p = b; const size_t* r = info;
    if (t != 0x1220 || r[0] % (getenv("MOCK_ALIGN") ? (size_t)atoi(getenv("MOCK_ALIGN")) : 128) || r[0] + r[1] > p->size) { *e = -13; return NULL; }
    mem* m = calloc(1, sizeof *m); m->size = r[1]; m->data = p->data + r[0]; m->sub = 1; *e = 0; return m;
}
int clEnqueueWriteBuffer(void* q, void* b, unsigned bl, size_t off, size_t n, const void* h, unsigned ne, const void* ev, void* oe) {
    (void)q; (void)bl; (void)ne; (void)ev; (void)oe; mem* m = b; if (off + n > m->size) return -30; memcpy(m->data + off, h, n); return 0; }
int clEnqueueReadBuffer(void* q, void* b, unsigned bl, size_t off, size_t n, void* h, unsigned ne, const void* ev, void* oe) {
    (void)q; (void)bl; (void)ne; (void)ev; (void)oe; mem* m = b; if (off + n > m->size) return -30; memcpy(h, m->data + off, n); return 0; }
int clEnqueueCopyBuffer(void* q, void* s, void* d, size_t so, size_t dof, size_t n, unsigned ne, const void* ev, void* oe) {
    (void)q; (void)ne; (void)ev; (void)oe; mem* a = s; mem* b = d;
    if (so + n > a->size || dof + n > b->size) return -30; memmove(b->data + dof, a->data + so, n); return 0; }
int clFinish(void* q) { (void)q; return 0; }
int clReleaseMemObject(void* b) { mem* m = b; if (!m) return 0; if (!m->sub) free(m->data); free(m); return 0; }
int clReleaseKernel(void* k) { free(k); return 0; }
int clReleaseProgram(void* p) { free(p); return 0; }
int clReleaseCommandQueue(void* q) { free(q); return 0; }
int clReleaseContext(void* c) { free(c); return 0; }
