/* Algorithm recovery corpus: open-addressing hash table */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CAP 32

typedef struct {
    char key[16];
    int value;
    int used;
} Slot;

static uint32_t hash_key(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static void ht_insert(Slot* t, const char* key, int value) {
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

static int ht_lookup(const Slot* t, const char* key) {
    uint32_t i = hash_key(key) % CAP;
    for (int n = 0; n < CAP; ++n) {
        if (t[i].used && strcmp(t[i].key, key) == 0) return t[i].value;
        i = (i + 1) % CAP;
    }
    return -1;
}

int main(void) {
    Slot table[CAP] = {0};
    ht_insert(table, "alpha", 1);
    ht_insert(table, "beta", 2);
    printf("%d %d\n", ht_lookup(table, "alpha"), ht_lookup(table, "beta"));
    return 0;
}
