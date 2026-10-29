/* ppc64 from the widened audit harness:
 * clang-18 --target=powerpc64-linux-gnu -O1 -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_entry -> audit7-ppc64-pie.
 * The divd divisor guard umax(y, 1) is an UNSIGNED compare of a signed
 * variable: it must not print as `y > 1 ? y : 1`. */
typedef long long i64;
#define NI __attribute__((noinline))
NI int f_s64(int a, int b, int c){ i64 x=((i64)a<<32)|(unsigned)b, y=(i64)c*977; i64 r = x>>(c&63); if(y) r += x/y - x%y; r += (x<y) + (x>y)*2; return (int)(r ^ (r>>32)); }
NI int f_entry(int a, int b, int c){ return f_s64(a, b, c); }
