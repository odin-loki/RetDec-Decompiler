#include <stdio.h>
static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) ++n;
    return n;
}
int main(void) {
    printf("%d\n", my_strlen("hello"));
    return 0;
}
