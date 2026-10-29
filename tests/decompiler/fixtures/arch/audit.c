/* Cross-arch audit program (built with rt32.c from the audit harness for 32-bit
 * targets: clang-18 --target=powerpc-linux-gnu -O1 -ffreestanding -fno-builtin
 * -nostdlib -fuse-ld=lld -Wl,-e,f_add8). */
typedef unsigned int u32; typedef unsigned long long u64; typedef long long i64;
int g_arr[16] = {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};
int g_acc;
#define NI __attribute__((noinline))
NI int f_add8(int a,int b,int c,int d,int e,int f,int g,int h){return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h;}
NI int f_sdiv(int a,int b){return b? a/b + a%b : -1;}
NI u32 f_udiv(u32 a,u32 b){return b? a/b ^ a%b : 7;}
NI int f_shr(int a,int s){return (a>>(s&31)) ^ (int)((u32)a>>(s&31));}
NI i64 f_mul64(i64 a,i64 b){return a*b + (a>>3) - (b<<5);}
NI u64 f_udiv64(u64 a,u64 b){return b? a/b + a%b : 0;}
NI int f_switch(int x){switch(x){case 0:return 11;case 1:return 22;case 2:return 35;case 3:return 47;case 5:return 51;case 8:return 89;default:return -x;}}
NI int f_loop(int n){int s=0; for(int i=0;i<n;i++){ s += g_arr[i&15]*i; if(s>1000) s-=999;} return s;}
NI int f_global(int x){g_acc += x; return g_acc*3;}
NI int f_minmax(int a,int b,int c){int m=a<b?a:b; int M=a>b?a:b; return (m<c?m:c)*100 + (M>c?M:c);}
NI int f_bits(u32 x){int c=0; while(x){c+=x&1; x>>=1;} return c;}
NI int f_sext(int x){signed char c=(signed char)x; short s=(short)x; return c + s + (unsigned char)x;}
NI int f_cmp64(i64 a,i64 b){return (a<b) + 2*((u64)a<(u64)b) + 4*(a==b);}
NI int f_rec(int n){return n<2? n : f_rec(n-1)+f_rec(n-2);}
NI int f_arr(int *p,int n){int s=0; for(int i=0;i<n;i++) s=s*31+p[i]; return s;}
NI int f_rot(u32 x,int r){r&=31; return (x<<r)|(x>>((32-r)&31));}
NI int f_clz(u32 x){return x? __builtin_clz(x):32;}
NI i64 f_neg(i64 a){return a<0? -a : a ^ 0x5555;}
