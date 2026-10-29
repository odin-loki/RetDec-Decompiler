/* ppc64 (big-endian ELFv1) from the widened audit harness:
 * clang-18 --target=powerpc64-linux-gnu -O1 -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_entry -> audit6-ppc64-pie.
 * lfd/lfs constants must not be byte-swapped; `mulli 977` is not an
 * address even though 977 lies in the header segment mapped at 0. */
typedef long long i64;
#define NI __attribute__((noinline))
NI int f_dbl(int a, int b){ double d=(double)a*1e-3 + b; d = d*d - (double)b/7.0; return (int)d; }
NI i64 f_mul(i64 a){ return a * 977; }
NI int f_entry(int a, int b){ return f_dbl(a, b) + (int)f_mul(b); }
