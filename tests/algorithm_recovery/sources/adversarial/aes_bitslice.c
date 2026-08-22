/* AES-128 SubBytes via GF(2^8) inversion + affine map. No S-box table, no AES-NI. */
#include <stdint.h>
#include <stdio.h>

static uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t p = 0;
	for (int i = 0; i < 8; ++i)
	{
		if (b & 1) p ^= a;
		uint8_t hi = (uint8_t)(a & 0x80u);
		a = (uint8_t)(a << 1);
		if (hi) a ^= 0x1bu;
		b = (uint8_t)(b >> 1);
	}
	return p;
}

static uint8_t gf_inv(uint8_t x)
{
	if (x == 0) return 0;
	uint8_t r = 1;
	uint8_t a = x;
	for (int e = 0; e < 7; ++e)
	{
		a = gf_mul(a, a);
		r = gf_mul(r, a);
	}
	return r;
}

static uint8_t sub_byte(uint8_t x)
{
	uint8_t y = gf_inv(x);
	uint8_t r = 0;
	for (int i = 0; i < 8; ++i)
	{
		uint8_t bit = (uint8_t)(((y >> i) ^ (y >> ((i + 4) % 8)) ^ (y >> ((i + 5) % 8)) ^ (y >> ((i + 6) % 8))
								 ^ (y >> ((i + 7) % 8)))
								& 1u);
		r |= (uint8_t)(bit << i);
	}
	return (uint8_t)(r ^ 0x63u);
}

static uint8_t xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0));
}

static void mix_columns(uint8_t s[16])
{
	for (int c = 0; c < 4; ++c)
	{
		uint8_t a0 = s[c];
		uint8_t a1 = s[4 + c];
		uint8_t a2 = s[8 + c];
		uint8_t a3 = s[12 + c];
		uint8_t t = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
		s[c] = (uint8_t)(a0 ^ t ^ xtime((uint8_t)(a0 ^ a1)));
		s[4 + c] = (uint8_t)(a1 ^ t ^ xtime((uint8_t)(a1 ^ a2)));
		s[8 + c] = (uint8_t)(a2 ^ t ^ xtime((uint8_t)(a2 ^ a3)));
		s[12 + c] = (uint8_t)(a3 ^ t ^ xtime((uint8_t)(a3 ^ a0)));
	}
}

static void shift_rows(uint8_t s[16])
{
	uint8_t t;
	t = s[4];
	s[4] = s[5];
	s[5] = s[6];
	s[6] = s[7];
	s[7] = t;
	t = s[8];
	s[8] = s[10];
	s[10] = t;
	t = s[9];
	s[9] = s[11];
	s[11] = t;
	t = s[15];
	s[15] = s[14];
	s[14] = s[13];
	s[13] = s[12];
	s[12] = t;
}

static void aes128_algebraic(uint8_t out[16], const uint8_t in[16], const uint8_t rk[176])
{
	uint8_t s[16];
	for (int i = 0; i < 16; ++i) s[i] = (uint8_t)(in[i] ^ rk[i]);
	for (int r = 1; r < 10; ++r)
	{
		for (int i = 0; i < 16; ++i) s[i] = sub_byte(s[i]);
		shift_rows(s);
		mix_columns(s);
		for (int i = 0; i < 16; ++i) s[i] ^= rk[16 * r + i];
	}
	for (int i = 0; i < 16; ++i) s[i] = sub_byte(s[i]);
	shift_rows(s);
	for (int i = 0; i < 16; ++i) out[i] = (uint8_t)(s[i] ^ rk[160 + i]);
}

int main(void)
{
	uint8_t in[16] = {0};
	uint8_t out[16] = {0};
	uint8_t rk[176] = {0};
	rk[0] = 0x2bu;
	aes128_algebraic(out, in, rk);
	printf("%02x\n", out[0]);
	return 0;
}
