#include <stdio.h>
#include <string.h>
#include <ctype.h>

int my_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

void my_toupper(char *s) {
    while (*s) { *s = toupper(*s); s++; }
}

int my_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

char *my_strrev(char *s) {
    int len = my_strlen(s);
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    }
    return s;
}

int main(void) {
    char buf[] = "Hello, World!";
    printf("len=%d\n", my_strlen(buf));
    my_toupper(buf);
    printf("upper=%s\n", buf);
    my_strrev(buf);
    printf("rev=%s\n", buf);
    printf("cmp=%d\n", my_strcmp("abc", "abd"));
    return 0;
}
