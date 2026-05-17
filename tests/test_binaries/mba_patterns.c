/* Mixed Boolean Arithmetic patterns: exercises mba_sub_optimizer */
#include <stdio.h>
#include <stdint.h>

/* MBA identity transformations that the optimizer should simplify */
/* x + y = (x ^ y) + 2*(x & y) */
/* x - y = x + (~y) + 1         */
/* x * y = ... (more complex)   */

/* Various bit manipulations that may become MBA patterns after optimization */
uint32_t add_mba(uint32_t x, uint32_t y) {
    return (x ^ y) + 2 * (x & y);
}

uint32_t sub_mba(uint32_t x, uint32_t y) {
    return x + (~y) + 1;
}

uint32_t and_mba(uint32_t x, uint32_t y) {
    return ((x | y) - (x ^ y) / 2);
}

/* Power of 2 operations */
uint32_t round_up_pow2(uint32_t v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    v++;
    return v;
}

/* Bit tricks */
int32_t abs_no_branch(int32_t x) {
    int32_t mask = x >> 31;
    return (x + mask) ^ mask;
}

uint32_t log2_floor(uint32_t x) {
    uint32_t r = 0;
    while (x >>= 1) r++;
    return r;
}

/* Sign detection without comparison */
int sgn(int32_t x) {
    return (x > 0) - (x < 0);
}

/* Swap without temp */
void swap_no_tmp(int *a, int *b) {
    *a ^= *b;
    *b ^= *a;
    *a ^= *b;
}

int main(void) {
    printf("add_mba(5,3)=%u\n", add_mba(5,3));
    printf("sub_mba(10,3)=%u\n", sub_mba(10,3));
    printf("round_up(13)=%u\n", round_up_pow2(13));
    printf("abs(-42)=%d abs(42)=%d\n", abs_no_branch(-42), abs_no_branch(42));
    printf("log2(64)=%u log2(100)=%u\n", log2_floor(64), log2_floor(100));
    printf("sgn(-5)=%d sgn(0)=%d sgn(7)=%d\n", sgn(-5), sgn(0), sgn(7));
    int a = 10, b = 20;
    swap_no_tmp(&a, &b);
    printf("after swap: a=%d b=%d\n", a, b);
    return 0;
}
