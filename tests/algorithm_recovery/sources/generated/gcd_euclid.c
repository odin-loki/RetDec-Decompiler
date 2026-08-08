#include <stdio.h>
static int gcd(int a, int b) {
    while (b) { int t = b; b = a % b; a = t; }
    return a;
}
int main(void) { printf("%d\n", gcd(48, 18)); return 0; }
