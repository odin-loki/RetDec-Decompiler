#include <stdio.h>
#include <stdint.h>
static uint32_t crc32_byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ (0xEDB88320u & (-(int)(crc & 1)));
    return crc;
}
int main(void) {
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_byte(c, 'A');
    printf("%08x\n", ~c);
    return 0;
}
