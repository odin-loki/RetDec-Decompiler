/* SIMD-like integer patterns, exercises array analysis (no FP/SSE) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Structure of arrays style — exercises array analysis */
typedef struct {
    int32_t x[256], y[256], z[256];
} Points3D;

void translate(Points3D *pts, int n, int32_t dx, int32_t dy, int32_t dz) {
    for (int i = 0; i < n; i++) {
        pts->x[i] += dx;
        pts->y[i] += dy;
        pts->z[i] += dz;
    }
}

int64_t dot_product(int32_t *a, int32_t *b, int n) {
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += (int64_t)a[i] * b[i];
    return sum;
}

/* SAXPY-like: y = a*x + y (integer version) */
void saxpy_int(int n, int32_t a, int32_t *x, int32_t *y) {
    for (int i = 0; i < n; i++) y[i] = a * x[i] + y[i];
}

/* Byte-level operations (exercises memset/memcpy) */
void zero_pad(char *dst, const char *src, int total_len) {
    int src_len = (int)strlen(src);
    if (src_len >= total_len) {
        memcpy(dst, src, total_len);
    } else {
        memcpy(dst, src, src_len);
        memset(dst + src_len, 0, total_len - src_len);
    }
}

/* Population count (bit manipulation) */
int popcount32(uint32_t x) {
    int n = 0;
    while (x) { n += (x & 1); x >>= 1; }
    return n;
}

uint32_t reverse_bits(uint32_t x) {
    uint32_t r = 0;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

int main(void) {
    int32_t a[8] = {1,2,3,4,5,6,7,8};
    int32_t b[8] = {8,7,6,5,4,3,2,1};
    printf("dot=%lld\n", (long long)dot_product(a, b, 8));
    
    saxpy_int(8, 2, a, b);
    for (int i = 0; i < 8; i++) printf("%d ", b[i]); printf("\n");
    
    Points3D pts;
    for (int i = 0; i < 4; i++) pts.x[i] = pts.y[i] = pts.z[i] = i;
    translate(&pts, 4, 10, 20, 30);
    printf("pt0: (%d,%d,%d)\n", pts.x[0], pts.y[0], pts.z[0]);
    
    char buf[16];
    zero_pad(buf, "Hi", 10);
    printf("padded: '%s'\n", buf);
    
    printf("popcount(0xff)=%d\n", popcount32(0xff));
    printf("reverse(0x12345678)=0x%08x\n", reverse_bits(0x12345678));
    return 0;
}
