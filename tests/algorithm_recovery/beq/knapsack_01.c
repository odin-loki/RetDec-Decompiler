/* BEQ-01 behavioural-equivalence program (see scripts/ci/check_behaviour_equivalence.py).
 * Inputs come from an LCG seeded by a volatile global so nothing folds to a
 * constant, and output goes through putchar only: no initialised local
 * arrays, no variadic calls, no division and masks rather than %, which the
 * decompiler does not yet carry across faithfully and which are not what this check is about. */
#include <stdio.h>
/* The functions under test.  The harness links the decompiled definitions
 * into a copy of this program built with -DBEQ_DRIVER, where these are weak
 * and so give way to them. */
#ifdef BEQ_DRIVER
#define BEQ_ENTRY __attribute__((weak, noinline))
#else
#define BEQ_ENTRY static __attribute__((noipa))
#endif
volatile unsigned beq_seed = 12345u;
static unsigned beq_state;
static __attribute__((noinline)) unsigned beq_rand(void) {
    beq_state = beq_state * 1103515245u + 12345u;
    return (beq_state >> 16) & 0x7fff;
}
static __attribute__((noinline)) void beq_put(int x) {
    unsigned u = x < 0 ? 0u - (unsigned)x : (unsigned)x;
    int shift = 28;
    if (x < 0) putchar('-');
    while (shift > 0 && (u >> shift) == 0) shift -= 4;
    for (; shift >= 0; shift -= 4) {
        unsigned d = (u >> shift) & 15;
        putchar(d < 10 ? (int)('0' + d) : (int)('a' + d - 10));
    }
    putchar(' ');
}

BEQ_ENTRY int knapsack(const int w[], const int v[], int n, int cap) {
    int dp[32] = {0};
    for (int i = 0; i < n; ++i)
        for (int c = cap; c >= w[i]; --c)
            if (dp[c-w[i]] + v[i] > dp[c]) dp[c] = dp[c-w[i]] + v[i];
    return dp[cap];
}
static int W[8], Val[8];
int main(void) {
    beq_state = beq_seed;
    for (int r = 0; r < 30; ++r) {
        int n = 1 + (int)(beq_rand() & 7);
        for (int i = 0; i < n; ++i) { W[i] = 1 + (int)(beq_rand() & 7); Val[i] = (int)(beq_rand() & 63); }
        beq_put(knapsack(W, Val, n, (int)(beq_rand() & 31)));
    }
    putchar('\n');
    return 0;
}
