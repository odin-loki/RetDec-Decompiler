#include <stdio.h>
#include <stdint.h>
#define M 64
static uint64_t bits;
static void bf_add(const char* s) {
    uint32_t h = 0;
    while (*s) h = h*131 + (unsigned char)*s++;
    bits |= (1ULL << (h % M));
}
static int bf_maybe(const char* s) {
    uint32_t h = 0;
    while (*s) h = h*131 + (unsigned char)*s++;
    return (bits >> (h % M)) & 1;
}
int main(void) {
    bf_add("key");
    printf("%d\n", bf_maybe("key"));
    return 0;
}
