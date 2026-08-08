#include <stdio.h>
static int my_atoi(const char* s) {
    int n = 0, sign = 1;
    if (*s == '-') { sign = -1; ++s; }
    while (*s >= '0' && *s <= '9') { n = n*10 + (*s - '0'); ++s; }
    return sign * n;
}
int main(void) { printf("%d\n", my_atoi("-1234")); return 0; }
