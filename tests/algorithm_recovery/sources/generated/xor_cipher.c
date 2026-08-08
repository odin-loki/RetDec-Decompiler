#include <stdio.h>
static void xor_cipher(unsigned char* buf, int n, unsigned char k) {
    for (int i = 0; i < n; ++i) buf[i] ^= k;
}
int main(void) {
    unsigned char msg[] = "data";
    xor_cipher(msg, 4, 0x5A);
    printf("%02x\n", msg[0]);
    return 0;
}
