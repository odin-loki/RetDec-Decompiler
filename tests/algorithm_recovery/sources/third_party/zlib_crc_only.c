/* Driver only. Labels are from upstream zlib crc32.c (no deflate). */
#include <stdio.h>
#include "zlib.h"

int main(void)
{
	const unsigned char d[] = "third-party zlib crc32 only";
	unsigned long crc = crc32(0L, d, sizeof d - 1);
	printf("%lu\n", crc);
	return (int)(crc & 255);
}
