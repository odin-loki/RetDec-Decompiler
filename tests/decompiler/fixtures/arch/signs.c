/* Signedness regression program: a signed division and remainder whose
 * divisor register is otherwise typed unsigned, and byte/halfword sign
 * extensions folded into aarch64 extended-register adds. */
#define NI __attribute__((noinline))
NI int f_sdiv(int a,int b){return b? a/b + a%b : -1;}
NI int f_sext(int x){signed char c=(signed char)x; short s=(short)x; return c + s + (unsigned char)x;}
int main(int argc, char **argv){ (void)argv; return f_sdiv(argc*-77, argc+2) + f_sext(argc*200); }
