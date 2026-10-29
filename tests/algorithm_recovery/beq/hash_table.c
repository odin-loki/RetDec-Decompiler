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

#include <stdint.h>
#include <string.h>
#define CAP 32
typedef struct { char key[16]; int value; int used; } Slot;
static uint32_t hash_key(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}
BEQ_ENTRY void ht_insert(Slot* t, const char* key, int value) {
    uint32_t i = hash_key(key) % CAP;
    for (int n = 0; n < CAP; ++n) {
        if (!t[i].used || strcmp(t[i].key, key) == 0) {
            strncpy(t[i].key, key, sizeof(t[i].key) - 1);
            t[i].value = value;
            t[i].used = 1;
            return;
        }
        i = (i + 1) % CAP;
    }
}
BEQ_ENTRY int ht_lookup(const Slot* t, const char* key) {
    uint32_t i = hash_key(key) % CAP;
    for (int n = 0; n < CAP; ++n) {
        if (t[i].used && strcmp(t[i].key, key) == 0) return t[i].value;
        i = (i + 1) % CAP;
    }
    return -1;
}
static Slot table[CAP];
static char K[10];
static void mkkey(void) { int n = 1 + (int)(beq_rand() & 7); for (int i = 0; i < n; ++i) K[i] = (char)('a' + beq_rand() & 3); K[n] = 0; }
int main(void) {
    beq_state = beq_seed;
    for (int r = 0; r < 40; ++r) { mkkey(); ht_insert(table, K, r); }
    for (int r = 0; r < 40; ++r) { mkkey(); beq_put(ht_lookup(table, K)); }
    putchar('\n');
    return 0;
}
