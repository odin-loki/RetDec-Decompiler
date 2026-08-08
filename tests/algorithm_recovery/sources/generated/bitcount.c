#include <stdio.h>
static int popcount(unsigned x) {
    int c = 0;
    while (x) { c += x & 1; x >>= 1; }
    return c;
}
int main(void) { printf("%d\n", popcount(0b101101)); return 0; }
