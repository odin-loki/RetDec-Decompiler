#include <stdio.h>
#include <stdint.h>

uint32_t popcount(uint32_t x) {
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0f0f0f0f;
    return (x * 0x01010101) >> 24;
}

uint32_t reverse_bits(uint32_t x) {
    x = ((x & 0xffff0000) >> 16) | ((x & 0x0000ffff) << 16);
    x = ((x & 0xff00ff00) >> 8)  | ((x & 0x00ff00ff) << 8);
    x = ((x & 0xf0f0f0f0) >> 4)  | ((x & 0x0f0f0f0f) << 4);
    x = ((x & 0xcccccccc) >> 2)  | ((x & 0x33333333) << 2);
    x = ((x & 0xaaaaaaaa) >> 1)  | ((x & 0x55555555) << 1);
    return x;
}

int highest_bit(uint32_t x) {
    int pos = -1;
    while (x) { pos++; x >>= 1; }
    return pos;
}

int main(void) {
    uint32_t vals[] = {0, 1, 0xdeadbeef, 0xffffffff, 256};
    for (int i = 0; i < 5; i++) {
        uint32_t v = vals[i];
        printf("0x%08x: pop=%u rev=0x%08x hb=%d\n",
               v, popcount(v), reverse_bits(v), highest_bit(v));
    }
    return 0;
}
