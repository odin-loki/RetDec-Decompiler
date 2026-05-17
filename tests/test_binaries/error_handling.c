/* Error codes, return value checking, void return patterns */
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef enum {
    OK = 0,
    ERR_NULL_PTR = -1,
    ERR_OUT_OF_RANGE = -2,
    ERR_OVERFLOW = -3,
    ERR_INVALID = -4,
} Status;

const char *status_msg(Status s) {
    switch (s) {
        case OK:              return "OK";
        case ERR_NULL_PTR:    return "null pointer";
        case ERR_OUT_OF_RANGE:return "out of range";
        case ERR_OVERFLOW:    return "overflow";
        case ERR_INVALID:     return "invalid";
        default:              return "unknown";
    }
}

Status safe_sqrt(double x, double *out) {
    if (!out) return ERR_NULL_PTR;
    if (x < 0) return ERR_INVALID;
    double guess = x;
    for (int i = 0; i < 100 && guess > 0; i++) {
        double next = (guess + x / guess) / 2.0;
        if (next >= guess) break;
        guess = next;
    }
    *out = (x == 0) ? 0 : guess;
    return OK;
}

Status safe_add_i32(int32_t a, int32_t b, int32_t *out) {
    if (!out) return ERR_NULL_PTR;
    int64_t r = (int64_t)a + b;
    if (r > INT32_MAX || r < INT32_MIN) return ERR_OVERFLOW;
    *out = (int32_t)r;
    return OK;
}

typedef struct { uint8_t *data; size_t len, cap; } Buffer;

Status buf_write(Buffer *b, const uint8_t *src, size_t n) {
    if (!b || !src) return ERR_NULL_PTR;
    if (b->len + n > b->cap) return ERR_OUT_OF_RANGE;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return OK;
}

uint8_t buf_read(Buffer *b, size_t idx, Status *err) {
    if (!b) { if (err) *err = ERR_NULL_PTR; return 0; }
    if (idx >= b->len) { if (err) *err = ERR_OUT_OF_RANGE; return 0; }
    if (err) *err = OK;
    return b->data[idx];
}

int main(void) {
    double r;
    Status s = safe_sqrt(2.0, &r);
    printf("sqrt(2)=%.6f status=%s\n", r, status_msg(s));
    
    s = safe_sqrt(-1.0, &r);
    printf("sqrt(-1) status=%s\n", status_msg(s));
    
    s = safe_sqrt(9.0, NULL);
    printf("sqrt(9,NULL) status=%s\n", status_msg(s));
    
    int32_t result;
    s = safe_add_i32(INT32_MAX, 1, &result);
    printf("overflow status=%s\n", status_msg(s));
    
    s = safe_add_i32(10, 20, &result);
    printf("10+20=%d status=%s\n", result, status_msg(s));
    
    uint8_t mem[64];
    Buffer buf = {mem, 0, sizeof(mem)};
    const uint8_t data[] = {1,2,3,4,5};
    buf_write(&buf, data, 5);
    Status err;
    printf("buf[2]=%d\n", buf_read(&buf, 2, &err));
    buf_read(&buf, 10, &err);
    printf("buf[10] status=%s\n", status_msg(err));
    return 0;
}
