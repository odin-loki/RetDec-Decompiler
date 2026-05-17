/* Ternary expressions and integer cast optimizations (avoid SSE) */
#include <stdio.h>
#include <stdint.h>
#include <limits.h>

/* Many ternary operators — exercises ternary_op_expr */
int clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

const char *sign_str(int x) {
    return x < 0 ? "negative" : (x == 0 ? "zero" : "positive");
}

const char *compare(int a, int b) {
    return a < b ? "less" : (a == b ? "equal" : "greater");
}

/* Integer widening / narrowing casts */
int64_t sign_extend_32(uint32_t x) {
    return (int64_t)(int32_t)x;
}

uint8_t saturate_add_u8(uint8_t a, uint8_t b) {
    uint16_t sum = (uint16_t)a + (uint16_t)b;
    return (uint8_t)(sum > 255 ? 255 : sum);
}

int64_t safe_mul(int32_t a, int32_t b) {
    return (int64_t)a * (int64_t)b;
}

/* Mixed type integer arithmetic */
int64_t weighted_sum(int *vals, int *weights, int n) {
    int64_t sum = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        sum += (int64_t)vals[i] * weights[i];
        wsum += weights[i];
    }
    return wsum > 0 ? sum / wsum : 0;
}

/* Conditional assignment chains */
int grade(int score) {
    return score >= 90 ? 4 :
           score >= 80 ? 3 :
           score >= 70 ? 2 :
           score >= 60 ? 1 : 0;
}

int main(void) {
    printf("clamp(15,0,10)=%d\n", clamp(15, 0, 10));
    printf("clamp(-5,0,10)=%d\n", clamp(-5, 0, 10));
    printf("clamp(5,0,10)=%d\n", clamp(5, 0, 10));
    printf("sign(-3)=%s sign(0)=%s sign(5)=%s\n", sign_str(-3), sign_str(0), sign_str(5));
    printf("compare(3,5)=%s\n", compare(3, 5));
    printf("sign_extend(0xffffffff)=%lld\n", sign_extend_32(0xffffffff));
    printf("sat_add(200,100)=%d\n", saturate_add_u8(200, 100));
    printf("safe_mul(100000,100000)=%lld\n", safe_mul(100000, 100000));
    int vs[] = {10, 20, 30};
    int ws[] = {2, 5, 3};
    printf("wsum=%lld\n", weighted_sum(vs, ws, 3));
    for (int s = 55; s <= 100; s += 5)
        printf("grade(%d)=%d ", s, grade(s));
    printf("\n");
    return 0;
}
