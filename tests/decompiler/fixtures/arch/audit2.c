/* Floats and varargs from the widened audit harness (x86-64 SSE):
 * clang-18 --target=x86_64-linux-gnu -O1 -ffreestanding -fno-builtin -nostdlib
 * -fuse-ld=lld -Wl,-e,f_entry. */
#include <stdarg.h>
typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#define NI __attribute__((noinline))
static int clampd(double d){ return d > 1e9 ? 1000000000 : d < -1e9 ? -1000000000 : (int)d; }
NI int f_fl(int a, int b){ float x=a*0.5f; float y = b ? (float)a/(float)b : 1.5f; return clampd((double)(x*y + 3.25f)); }
static NI int vsum(int n, ...){ va_list ap; va_start(ap,n); int s=0; for(int i=0;i<n;i++) s = s*3 + va_arg(ap,int); va_end(ap); return s; }
NI int f_va(int a, int b, int c){ return vsum(3,a,b,c) + vsum(1,a^b) + vsum(5,1,2,3,4,c); }
NI int f_entry(int a, int b){ return f_fl(a, b) + f_va(a, b, a); }
