#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 16

typedef struct Entry {
    char key[32];
    int value;
    struct Entry *next;
} Entry;

typedef struct {
    Entry *buckets[BUCKETS];
} HashMap;

static unsigned hash(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 ^ (unsigned char)*s++;
    return h % BUCKETS;
}

void hm_set(HashMap *m, const char *key, int val) {
    unsigned h = hash(key);
    for (Entry *e = m->buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) { e->value = val; return; }
    }
    Entry *e = malloc(sizeof(Entry));
    strncpy(e->key, key, 31); e->key[31] = '\0';
    e->value = val;
    e->next = m->buckets[h];
    m->buckets[h] = e;
}

int hm_get(HashMap *m, const char *key, int *out) {
    unsigned h = hash(key);
    for (Entry *e = m->buckets[h]; e; e = e->next)
        if (strcmp(e->key, key) == 0) { *out = e->value; return 1; }
    return 0;
}

int main(void) {
    HashMap m = {0};
    hm_set(&m, "foo", 42);
    hm_set(&m, "bar", 7);
    hm_set(&m, "baz", 99);
    hm_set(&m, "foo", 100);
    int v;
    const char *keys[] = {"foo", "bar", "baz", "qux"};
    for (int i = 0; i < 4; i++) {
        if (hm_get(&m, keys[i], &v))
            printf("%s=%d\n", keys[i], v);
        else
            printf("%s=not found\n", keys[i]);
    }
    return 0;
}
