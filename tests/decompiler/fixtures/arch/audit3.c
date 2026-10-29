/* Struct arrays and 64-bit math from the widened audit harness:
 * clang-18 --target=x86_64-linux-gnu -O<1|2> -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_entry (audit3-x86_64-pie is -O2,
 * audit3-x86_64-pie-o1 is -O1). */
typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
#define NI __attribute__((noinline))
struct P { int x; short y; char z; int w[3]; };
struct P g_ps[4];
NI int f_st_arr(int i, int v){ g_ps[i&3].y=(short)v; g_ps[(i+1)&3].w[2]=v*3; g_ps[(i+2)&3].z=(char)v; int s=0; for(int k=0;k<4;k++) s += g_ps[k].y + g_ps[k].w[2] + g_ps[k].z; return s; }
NI int f_s64(int a, int b, int c){ i64 x=((i64)a<<32)|(u32)b, y=(i64)c*977; i64 r = x>>(c&63); if(y) r += x/y - x%y; r += (x<y) + (x>y)*2; return (int)(r ^ (r>>32)); }
NI int f_entry(int a, int b){ return f_st_arr(a, b) + f_s64(a, b, a); }
