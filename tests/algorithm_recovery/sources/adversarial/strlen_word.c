/* Word-at-a-time strlen (SWAR). No per-byte null compare loop. */
#include <stdio.h>
#include <stdint.h>

static unsigned long strlen_word(const char* s)
{
	const unsigned char* p = (const unsigned char*)s;
	while (((uintptr_t)p & 7u) != 0)
	{
		if (*p == 0) return (unsigned long)(p - (const unsigned char*)s);
		++p;
	}
	const uint64_t* w = (const uint64_t*)p;
	const uint64_t himagic = 0x8080808080808080ull;
	const uint64_t lomagic = 0x0101010101010101ull;
	for (;;)
	{
		uint64_t v = *w;
		if (((v - lomagic) & ~v & himagic) != 0)
		{
			const unsigned char* q = (const unsigned char*)w;
			for (int i = 0; i < 8; ++i)
			{
				if (q[i] == 0) return (unsigned long)(q + i - (const unsigned char*)s);
			}
		}
		++w;
	}
}

int main(void)
{
	printf("%lu\n", strlen_word("adversarial"));
	return 0;
}
