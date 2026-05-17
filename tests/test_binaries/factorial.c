#include <stdio.h>
#include <stdint.h>

uint64_t factorial(uint32_t n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    for (int i = 0; i <= 10; i++) {
        printf("%d! = %lu\n", i, factorial(i));
    }
    return 0;
}
