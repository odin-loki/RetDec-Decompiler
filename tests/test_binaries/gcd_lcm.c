#include <stdio.h>
#include <stdint.h>

uint64_t gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = b; b = a % b; a = t; }
    return a;
}

uint64_t lcm(uint64_t a, uint64_t b) {
    return a / gcd(a, b) * b;
}

int is_prime(uint32_t n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if (n % 2 == 0) return 0;
    for (uint32_t i = 3; (uint64_t)i * i <= n; i += 2)
        if (n % i == 0) return 0;
    return 1;
}

int main(void) {
    printf("gcd(48,18)=%lu lcm(4,6)=%lu\n", gcd(48,18), lcm(4,6));
    printf("Primes up to 50: ");
    for (int i = 2; i <= 50; i++)
        if (is_prime(i)) printf("%d ", i);
    printf("\n");
    return 0;
}
