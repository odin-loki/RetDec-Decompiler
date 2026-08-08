#include <stdio.h>
static void my_memset(unsigned char* p, unsigned char v, int n) {
    for (int i = 0; i < n; ++i) p[i] = v;
}
int main(void) {
    unsigned char b[4];
    my_memset(b, 0xAB, 4);
    printf("%02x\n", b[0]);
    return 0;
}
