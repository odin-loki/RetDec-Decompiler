/* N64 mips floating point from the widened audit harness:
 * clang-18 --target=mips64el-linux-gnuabi64 -O1 -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_entry -> audit5-mips64el-pie.
 * mtc1+cvt.s.w, a 4-byte lwc1 float literal, and mul.s rounding. */
typedef unsigned int u32;
#define NI __attribute__((noinline))
NI int f_u2f(int a){ return (int)((float)((u32)a >> 8) * 0.25f); }
NI int f_fl(int a, int b){ float x=a*0.5f; float y = b ? (float)a/(float)b : 1.5f; return (int)(x*y + 3.25f); }
NI int f_entry(int a, int b){ return f_u2f(a) + f_fl(a, b); }
