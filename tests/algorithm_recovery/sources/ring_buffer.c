/* Algorithm recovery corpus: ring buffer */
#include <stdio.h>
#include <string.h>

#define N 8

typedef struct {
    char data[N];
    int head;
    int tail;
    int count;
} RingBuf;

static void rb_push(RingBuf* r, char c) {
    if (r->count >= N) return;
    r->data[r->tail] = c;
    r->tail = (r->tail + 1) % N;
    ++r->count;
}

static int rb_pop(RingBuf* r, char* out) {
    if (r->count == 0) return 0;
    *out = r->data[r->head];
    r->head = (r->head + 1) % N;
    --r->count;
    return 1;
}

int main(void) {
    RingBuf rb = {0};
    const char* msg = "abc";
    for (int i = 0; msg[i]; ++i) rb_push(&rb, msg[i]);
    char c;
    while (rb_pop(&rb, &c)) putchar(c);
    putchar('\n');
    return 0;
}
