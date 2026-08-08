#include <stdio.h>
static unsigned fib(unsigned n) {
    unsigned a = 0, b = 1;
    for (unsigned i = 0; i < n; ++i) {
        unsigned t = a + b; a = b; b = t;
    }
    return a;
}
int main(void) { printf("%u\n", fib(10)); return 0; }
