/* Driver only. Labels are from upstream zlib crc32.c / compress.c. */
#include <stdio.h>
#include "zlib.h"

int main(void)
{
	const unsigned char d[] = "third-party zlib corpus";
	unsigned long crc = crc32(0L, d, sizeof d - 1);
	unsigned long dest_len = 128;
	unsigned char out[128];
	if (compress(out, &dest_len, d, sizeof d - 1) != Z_OK) return 2;
	printf("%lu %lu\n", crc, dest_len);
	return (int)(crc & 255);
}
