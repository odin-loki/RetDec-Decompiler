/* AES-128 encrypt round via AES-NI intrinsics (wmmintrin). */
#include <stdio.h>
#include <wmmintrin.h>

static __m128i aes_ni_encrypt(__m128i block, const __m128i* rk)
{
	block = _mm_xor_si128(block, rk[0]);
	block = _mm_aesenc_si128(block, rk[1]);
	block = _mm_aesenc_si128(block, rk[2]);
	block = _mm_aesenc_si128(block, rk[3]);
	block = _mm_aesenc_si128(block, rk[4]);
	block = _mm_aesenc_si128(block, rk[5]);
	block = _mm_aesenc_si128(block, rk[6]);
	block = _mm_aesenc_si128(block, rk[7]);
	block = _mm_aesenc_si128(block, rk[8]);
	block = _mm_aesenc_si128(block, rk[9]);
	return _mm_aesenclast_si128(block, rk[10]);
}

int main(void)
{
	__m128i rk[11];
	for (int i = 0; i < 11; ++i) rk[i] = _mm_setzero_si128();
	__m128i block = _mm_setzero_si128();
	block = aes_ni_encrypt(block, rk);
	unsigned char out[16];
	_mm_storeu_si128((__m128i*)out, block);
	printf("%02x\n", out[0]);
	return 0;
}
