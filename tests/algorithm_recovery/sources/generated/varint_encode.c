#include <stdio.h>
#include <stdint.h>
static int encode_varint(uint32_t v, unsigned char* out) {
    int i = 0;
    while (v >= 0x80) { out[i++] = (unsigned char)((v & 0x7F) | 0x80); v >>= 7; }
    out[i++] = (unsigned char)v;
    return i;
}
int main(void) {
    unsigned char buf[8];
    int n = encode_varint(300, buf);
    printf("%d %d\n", n, buf[0]);
    return 0;
}
