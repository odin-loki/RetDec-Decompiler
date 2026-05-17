/* Error handling with errno-style patterns (avoid setjmp/FP) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Integer sqrt via Newton-Raphson */
Status safe_isqrt(int64_t x, int64_t *out) {
    if (!out) return ERR_NULL_PTR;
    if (x < 0) return ERR_INVALID;
    if (x == 0) { *out = 0; return OK; }
    int64_t guess = x;
    for (int i = 0; i < 64; i++) {
        int64_t next = (guess + x / guess) / 2;
        if (next >= guess) break;
        guess = next;
    }
    *out = guess;
    return OK;
}

Status safe_add_i32(int32_t a, int32_t b, int32_t *out) {
    if (!out) return ERR_NULL_PTR;
    int64_t r = (int64_t)a + b;
    if (r > INT32_MAX || r < INT32_MIN) return ERR_OVERFLOW;
    *out = (int32_t)r;
    return OK;
}

Status safe_div(int32_t a, int32_t b, int32_t *out) {
    if (!out) return ERR_NULL_PTR;
    if (b == 0) return ERR_INVALID;
    if (a == INT32_MIN && b == -1) return ERR_OVERFLOW;
    *out = a / b;
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
    int64_t r;
    Status s = safe_isqrt(144, &r);
    printf("isqrt(144)=%lld status=%s\n", (long long)r, status_msg(s));
    
    s = safe_isqrt(-1, &r);
    printf("isqrt(-1) status=%s\n", status_msg(s));
    
    s = safe_isqrt(9, NULL);
    printf("isqrt(9,NULL) status=%s\n", status_msg(s));
    
    int32_t result;
    s = safe_add_i32(INT32_MAX, 1, &result);
    printf("overflow status=%s\n", status_msg(s));
    
    s = safe_add_i32(10, 20, &result);
    printf("10+20=%d status=%s\n", result, status_msg(s));
    
    s = safe_div(10, 0, &result);
    printf("10/0 status=%s\n", status_msg(s));
    
    s = safe_div(10, 3, &result);
    printf("10/3=%d status=%s\n", result, status_msg(s));
    
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
