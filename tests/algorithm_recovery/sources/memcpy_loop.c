/* Algorithm recovery corpus: memcpy-style loop */
#include <stddef.h>
#include <stdio.h>

static void copy_bytes(unsigned char* dst, const unsigned char* src, size_t n) {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
}

int main(void) {
    unsigned char a[] = "hello";
    unsigned char b[8] = {0};
    copy_bytes(b, a, 5);
    printf("%s\n", b);
    return 0;
}
