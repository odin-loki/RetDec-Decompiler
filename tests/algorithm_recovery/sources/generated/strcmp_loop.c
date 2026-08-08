#include <stdio.h>
static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}
int main(void) {
    printf("%d\n", my_strcmp("abc", "abd"));
    return 0;
}
