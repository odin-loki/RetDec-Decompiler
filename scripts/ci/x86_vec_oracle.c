/**
 * @file scripts/ci/x86_vec_oracle.c
 * @brief Executes the x86 instructions that read XMM registers and write one.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 *
 * The fourth oracle shape. x86_flag_oracle.c and x86_wide_oracle.c pass their
 * operands in general-purpose registers; x86_sse_oracle.c reads XMM and writes
 * a general-purpose register or the flags. This one reads XMM and writes XMM,
 * which is most of the integer SIMD instruction set.
 *
 * Both 64-bit halves of the destination are carried, before and after, so a
 * translator that writes the wrong lane -- or the right value to the wrong
 * half -- is a mismatch rather than something nobody looked at.
 *
 * Two things here are traps rather than transcription:
 *
 *   the shifts     take their count from the WHOLE low quadword of the second
 *                  operand, not per lane, and a count at or past the element
 *                  width gives zero (or, for the arithmetic right shifts, the
 *                  sign bit broadcast) rather than an undefined result. LLVM's
 *                  shl is poison there, so the count has to be clamped.
 *   the widenings  read only the LOW elements of their source. pmovsxbq reads
 *                  two bytes and writes two quadwords; everything above the
 *                  low sixteen bits of the source is untouched input that a
 *                  translator must not read.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t lo, hi; } Out;

#define FORM(name, insn)                                                      \
  static Out name(const uint64_t* a, const uint64_t* b) {                     \
    uint64_t r[2] = {0, 0};                                                   \
    __asm__ volatile("movdqu %1, %%xmm0\n\t"                                  \
                     "movdqu %2, %%xmm1\n\t"                                  \
                     insn "\n\t"                                              \
                     "movdqu %%xmm0, %0"                                      \
                     : "=m"(*r) : "m"(*a), "m"(*b) : "xmm0", "xmm1");         \
    Out o = { r[0], r[1] }; return o;                                         \
  }

FORM(o_psllw, "psllw %%xmm1, %%xmm0")
FORM(o_pslld, "pslld %%xmm1, %%xmm0")
FORM(o_psllq, "psllq %%xmm1, %%xmm0")
FORM(o_psrlw, "psrlw %%xmm1, %%xmm0")
FORM(o_psrld, "psrld %%xmm1, %%xmm0")
FORM(o_psrlq, "psrlq %%xmm1, %%xmm0")
FORM(o_psraw, "psraw %%xmm1, %%xmm0")
FORM(o_psrad, "psrad %%xmm1, %%xmm0")

FORM(o_pmovsxbw, "pmovsxbw %%xmm1, %%xmm0")
FORM(o_pmovsxbd, "pmovsxbd %%xmm1, %%xmm0")
FORM(o_pmovsxbq, "pmovsxbq %%xmm1, %%xmm0")
FORM(o_pmovsxwd, "pmovsxwd %%xmm1, %%xmm0")
FORM(o_pmovsxwq, "pmovsxwq %%xmm1, %%xmm0")
FORM(o_pmovsxdq, "pmovsxdq %%xmm1, %%xmm0")
FORM(o_pmovzxbw, "pmovzxbw %%xmm1, %%xmm0")
FORM(o_pmovzxbd, "pmovzxbd %%xmm1, %%xmm0")
FORM(o_pmovzxbq, "pmovzxbq %%xmm1, %%xmm0")
FORM(o_pmovzxwd, "pmovzxwd %%xmm1, %%xmm0")
FORM(o_pmovzxwq, "pmovzxwq %%xmm1, %%xmm0")
FORM(o_pmovzxdq, "pmovzxdq %%xmm1, %%xmm0")

typedef Out (*Fn)(const uint64_t*, const uint64_t*);

enum Kind { K_SHIFT, K_WIDEN };

static struct { const char* name; Fn fn; enum Kind kind; unsigned elem; } TBL[] = {
	{"psllw", o_psllw, K_SHIFT, 16}, {"pslld", o_pslld, K_SHIFT, 32},
	{"psllq", o_psllq, K_SHIFT, 64}, {"psrlw", o_psrlw, K_SHIFT, 16},
	{"psrld", o_psrld, K_SHIFT, 32}, {"psrlq", o_psrlq, K_SHIFT, 64},
	{"psraw", o_psraw, K_SHIFT, 16}, {"psrad", o_psrad, K_SHIFT, 32},
	{"pmovsxbw", o_pmovsxbw, K_WIDEN, 8},  {"pmovsxbd", o_pmovsxbd, K_WIDEN, 8},
	{"pmovsxbq", o_pmovsxbq, K_WIDEN, 8},  {"pmovsxwd", o_pmovsxwd, K_WIDEN, 16},
	{"pmovsxwq", o_pmovsxwq, K_WIDEN, 16}, {"pmovsxdq", o_pmovsxdq, K_WIDEN, 32},
	{"pmovzxbw", o_pmovzxbw, K_WIDEN, 8},  {"pmovzxbd", o_pmovzxbd, K_WIDEN, 8},
	{"pmovzxbq", o_pmovzxbq, K_WIDEN, 8},  {"pmovzxwd", o_pmovzxwd, K_WIDEN, 16},
	{"pmovzxwq", o_pmovzxwq, K_WIDEN, 16}, {"pmovzxdq", o_pmovzxdq, K_WIDEN, 32},
};

/* random() gives 31 bits, so the obvious shift-and-xor leaves bit 31 and bit
   63 clear in every draw -- the flaw that hid IDIV's sign bug in Batch AA.
   Five 13-bit chunks cover all 64. */
static uint64_t rnd64(void)
{
	uint64_t x = 0;
	for (int i = 0; i < 5; ++i) { x = (x << 13) ^ (uint64_t)(random() & 0x1fff); }
	return x;
}

/* Counts are drawn from a pool that straddles the element width, because
   "at or past the width gives zero" is the half of the shift semantics that
   a plain vector shl gets wrong. */
static uint64_t shiftCount(unsigned elem)
{
	switch (random() % 8) {
		case 0:  return 0;
		case 1:  return 1;
		case 2:  return elem - 1;
		case 3:  return elem;            /* exactly the width: all zero */
		case 4:  return elem + 1;
		case 5:  return 255;
		case 6:  return (uint64_t)1 << 40;   /* enormous, still just "too big" */
		default: return (uint64_t)(random() % elem);
	}
}

int main(int argc, char** argv)
{
	long n = argc > 1 ? atol(argv[1]) : 200;
	srandom(argc > 2 ? (unsigned)atol(argv[2]) : 20240404u);

	printf("# name x0lo x0hi x1lo x1hi rlo rhi\n");
	for (size_t k = 0; k < sizeof TBL / sizeof TBL[0]; ++k) {
		for (long i = 0; i < n; ++i) {
			uint64_t a[2], b[2];
			a[0] = rnd64(); a[1] = rnd64();
			if (TBL[k].kind == K_SHIFT) {
				b[0] = shiftCount(TBL[k].elem);
				/* The upper quadword of the count operand is ignored by the
				   hardware. Filling it with noise is how a translator that
				   reads it shows up. */
				b[1] = rnd64();
			} else {
				b[0] = rnd64(); b[1] = rnd64();
			}
			Out o = TBL[k].fn(a, b);
			printf("%s|%llu|%llu|%llu|%llu|%llu|%llu\n", TBL[k].name,
			       (unsigned long long)a[0], (unsigned long long)a[1],
			       (unsigned long long)b[0], (unsigned long long)b[1],
			       (unsigned long long)o.lo, (unsigned long long)o.hi);
		}
	}
	return 0;
}
