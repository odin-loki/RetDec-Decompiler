#include <stdio.h>
static int rle_encode(const char* in, char* out) {
    int n = 0, o = 0;
    while (in[n]) {
        char c = in[n]; int run = 1;
        while (in[n+run] == c) ++run;
        out[o++] = c; out[o++] = (char)('0' + run);
        n += run;
    }
    out[o] = '\0';
    return o;
}
int main(void) {
    char out[32];
    rle_encode("aaabbc", out);
    printf("%s\n", out);
    return 0;
}
