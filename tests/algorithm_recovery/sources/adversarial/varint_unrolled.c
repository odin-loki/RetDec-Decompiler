/* Protobuf-style varint without a loop (no back-edge). Four explicit chunks. */
#include <stdio.h>
#include <stdint.h>

static uint32_t varint_unrolled(const unsigned char* p, int* consumed)
{
	uint32_t x = p[0] & 0x7fu;
	if ((p[0] & 0x80u) == 0)
	{
		*consumed = 1;
		return x;
	}
	x |= (uint32_t)(p[1] & 0x7fu) << 7;
	if ((p[1] & 0x80u) == 0)
	{
		*consumed = 2;
		return x;
	}
	x |= (uint32_t)(p[2] & 0x7fu) << 14;
	if ((p[2] & 0x80u) == 0)
	{
		*consumed = 3;
		return x;
	}
	x |= (uint32_t)(p[3] & 0x7fu) << 21;
	*consumed = 4;
	return x;
}

int main(void)
{
	const unsigned char buf[] = {0xac, 0x02};
	int n = 0;
	printf("%u %d\n", varint_unrolled(buf, &n), n);
	return 0;
}
