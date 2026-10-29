/* aarch64 floating point from the widened audit harness:
 * clang-18 --target=aarch64-linux-gnu -O1 -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_entry -> audit4-aarch64-pie.
 * fcmp NZCV order, fixed-point ucvtf, and an 8-byte double literal. */
typedef unsigned int u32;
#define NI __attribute__((noinline))
static int clampd(double d){ return d > 1e9 ? 1000000000 : d < -1e9 ? -1000000000 : (int)d; }
NI int f_dbl(int a, int b){ double d=(double)a*1e-3 + b; d = d*d - (double)b/7.0; return clampd(d); }
NI int f_u2f(int a){ return (int)((float)((u32)a >> 8) * 0.25f); }
NI int f_entry(int a, int b){ return f_dbl(a, b) + f_u2f(b); }
