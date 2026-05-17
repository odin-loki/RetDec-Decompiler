/* Exercises char_array_to_string_optimizer: global char arrays, string ops */
#include <stdio.h>
#include <string.h>

/* Global char arrays — key trigger for char_array_to_string_optimizer */
static char greeting[] = "Hello, World!";
static char days[7][10] = {"Monday","Tuesday","Wednesday","Thursday","Friday","Saturday","Sunday"};
static const char hex_chars[] = "0123456789abcdef";

void print_hex(unsigned char *buf, int len) {
    for (int i = 0; i < len; i++) {
        putchar(hex_chars[(buf[i] >> 4) & 0xf]);
        putchar(hex_chars[buf[i] & 0xf]);
    }
    putchar('\n');
}

char *rotate_greeting(int n) {
    static char rotated[64];
    int len = strlen(greeting);
    n = ((n % len) + len) % len;
    for (int i = 0; i < len; i++)
        rotated[i] = greeting[(i + n) % len];
    rotated[len] = '\0';
    return rotated;
}

int main(void) {
    printf("%s\n", greeting);
    for (int i = 0; i < 7; i++)
        printf("%d: %s\n", i, days[i]);
    unsigned char data[] = {0xde, 0xad, 0xbe, 0xef};
    print_hex(data, 4);
    printf("%s\n", rotate_greeting(3));
    return 0;
}
