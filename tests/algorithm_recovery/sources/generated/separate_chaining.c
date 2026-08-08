#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define B 8
typedef struct E { char k[8]; int v; struct E* n; } E;
static E* buckets[B];
static unsigned h(const char* s) {
    unsigned x = 0; while (*s) x = x*31 + (unsigned char)*s++;
    return x % B;
}
static void put(const char* k, int v) {
    unsigned i = h(k);
    E* e = (E*)malloc(sizeof(E));
    strncpy(e->k, k, 7); e->v = v; e->n = buckets[i]; buckets[i] = e;
}
int main(void) { put("x", 42); printf("%d\n", buckets[h("x")]->v); return 0; }
