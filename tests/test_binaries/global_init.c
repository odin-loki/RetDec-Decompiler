/* Global constructors, static vars, large initializer lists (no trig/FP) */
#include <stdio.h>
#include <string.h>

/* Large global array with initializer list */
static int fibs[30];
static unsigned char xor_table[256];
static unsigned char crc_table[256];

static void init_fibs(void) {
    fibs[0] = 0; fibs[1] = 1;
    for (int i = 2; i < 30; i++)
        fibs[i] = fibs[i-1] + fibs[i-2];
}

static void init_xor(void) {
    for (int i = 0; i < 256; i++)
        xor_table[i] = (unsigned char)(i ^ 0x5A);
}

static void init_crc(void) {
    for (int i = 0; i < 256; i++) {
        unsigned int c = (unsigned int)i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = (unsigned char)(c & 0xff);
    }
}

void cipher(unsigned char *buf, int n) {
    for (int i = 0; i < n; i++)
        buf[i] = xor_table[buf[i]];
}

unsigned int crc32_simple(const unsigned char *data, int n) {
    unsigned int crc = 0xFFFFFFFF;
    for (int i = 0; i < n; i++)
        crc = (crc >> 8) ^ crc_table[(crc ^ data[i]) & 0xff];
    return crc ^ 0xFFFFFFFF;
}

int fib_sum(int n) {
    int s = 0;
    for (int i = 0; i < n && i < 30; i++)
        s += fibs[i];
    return s;
}

int main(void) {
    init_fibs();
    init_xor();
    init_crc();
    
    printf("fib[20]=%d\n", fibs[20]);
    printf("fib_sum(10)=%d\n", fib_sum(10));
    
    unsigned char msg[] = "Secret message!";
    int len = strlen((char*)msg);
    cipher(msg, len);
    printf("encrypted: ");
    for (int i = 0; i < len; i++) printf("%02x", msg[i]);
    printf("\n");
    cipher(msg, len); /* decrypt */
    printf("decrypted: %s\n", msg);
    
    const unsigned char data[] = {1,2,3,4,5};
    printf("crc32=0x%08x\n", crc32_simple(data, 5));
    return 0;
}
