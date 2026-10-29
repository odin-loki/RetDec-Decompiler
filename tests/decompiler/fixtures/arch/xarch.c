/* Cross-arch regression program: a global int table indexed in a loop, a
 * compare against an immediate that falls inside a PIE's low headers, and
 * a self-recursive call. */
int g_arr[16] = {3,1,4,1,5,9,2,6,5,3,5,8,9,7,9,3};
#define NI __attribute__((noinline))
NI int f_loop(int n){int s=0; for(int i=0;i<n;i++){ s += g_arr[i&15]*i; if(s>1000) s-=999;} return s;}
NI int f_rec(int n){return n<2? n : f_rec(n-1)+f_rec(n-2);}
NI long long f_neg(long long a){return a<0? -a : a ^ 0x5555;}
int main(int argc, char **argv){ (void)argv; return f_loop(argc*13) + f_rec(argc+5) + (int)f_neg(argc - 9); }
