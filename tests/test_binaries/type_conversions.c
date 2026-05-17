/* Integer type conversions: widening, narrowing, ptr casts (no SSE) */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* Integer widening and narrowing */
uint64_t widen_u32(uint32_t x) { return (uint64_t)x; }
uint32_t narrow_u64(uint64_t x) { return (uint32_t)(x & 0xffffffff); }
int64_t  widen_i32(int32_t x)   { return (int64_t)x; }
int32_t  narrow_i64(int64_t x)  { return (int32_t)x; }
uint8_t  narrow_u32_u8(uint32_t x) { return (uint8_t)(x & 0xff); }

/* Bit-level type punning via memcpy (no UB) */
uint32_t u32_from_bytes(const uint8_t *b) {
    uint32_t v;
    memcpy(&v, b, 4);
    return v;
}

void bytes_from_u32(uint32_t v, uint8_t *b) {
    memcpy(b, &v, 4);
}

/* Multi-step conversions with intermediate types */
uint16_t pack_bytes(uint8_t hi, uint8_t lo) {
    return ((uint16_t)hi << 8) | (uint16_t)lo;
}

uint8_t unpack_hi(uint16_t v) { return (uint8_t)(v >> 8); }
uint8_t unpack_lo(uint16_t v) { return (uint8_t)(v & 0xff); }

/* Pointer arithmetic with different integer types */
int64_t array_sum_u8(const uint8_t *data, size_t n) {
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)data[i];
    return sum;
}

int64_t array_sum_i16(const int16_t *data, size_t n) {
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) sum += (int64_t)data[i];
    return sum;
}

int main(void) {
    printf("widen(0xffffffff)=%llu\n", (unsigned long long)widen_u32(0xffffffff));
    printf("narrow(0x100000001)=%u\n", narrow_u64(0x100000001ULL));
    printf("widen_i32(-1)=%lld\n", (long long)widen_i32(-1));
    printf("narrow_i64(0x1ffffffff)=%d\n", narrow_i64(0x1ffffffffLL));
    
    uint8_t be[] = {0xde, 0xad, 0xbe, 0xef};
    printf("u32_from_bytes=0x%08x\n", u32_from_bytes(be));
    uint8_t out[4];
    bytes_from_u32(0x12345678, out);
    printf("bytes: %02x %02x %02x %02x\n", out[0], out[1], out[2], out[3]);
    
    uint16_t packed = pack_bytes(0xAB, 0xCD);
    printf("packed=0x%04x hi=0x%02x lo=0x%02x\n", packed, unpack_hi(packed), unpack_lo(packed));
    
    uint8_t u8data[] = {1, 2, 3, 4, 5, 6, 7, 8};
    printf("sum_u8=%lld\n", (long long)array_sum_u8(u8data, 8));
    int16_t i16data[] = {-1, 2, -3, 4, -5, 6, -7, 8};
    printf("sum_i16=%lld\n", (long long)array_sum_i16(i16data, 8));
    return 0;
}
