/* Copy-propagation regression program: an unrolled-by-4 loop whose trip
 * count (n & ~3) is copied into the remainder loop's start index after the
 * main loop; the copy must not be folded back over the trip count. */
#define NI __attribute__((noinline))
NI int f_arr(int *p,int n){int s=0; for(int i=0;i<n;i++) s=s*31+p[i]; return s;}
int main(int argc, char **argv){ return f_arr((int *)argv, argc); }
