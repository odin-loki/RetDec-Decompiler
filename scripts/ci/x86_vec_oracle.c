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

/* ---------------------------------------- saturating and widening integer */

/* These are the ones a plain vector add gets wrong in a way that looks right:
   PADDSB of 127 and 1 is 127, not -128. LLVM has intrinsics that say exactly
   this -- llvm.sadd.sat and friends -- so the translation is a name rather
   than a hand-rolled clamp, but only if the right one is picked. */

FORM(o_paddsb,  "paddsb  %%xmm1, %%xmm0")
FORM(o_paddsw,  "paddsw  %%xmm1, %%xmm0")
FORM(o_paddusb, "paddusb %%xmm1, %%xmm0")
FORM(o_paddusw, "paddusw %%xmm1, %%xmm0")
FORM(o_psubsb,  "psubsb  %%xmm1, %%xmm0")
FORM(o_psubsw,  "psubsw  %%xmm1, %%xmm0")
FORM(o_psubusb, "psubusb %%xmm1, %%xmm0")
FORM(o_psubusw, "psubusw %%xmm1, %%xmm0")

FORM(o_pmullw,  "pmullw  %%xmm1, %%xmm0")
FORM(o_pmulhw,  "pmulhw  %%xmm1, %%xmm0")
FORM(o_pmulhuw, "pmulhuw %%xmm1, %%xmm0")
FORM(o_pmulld,  "pmulld  %%xmm1, %%xmm0")
FORM(o_pmuludq, "pmuludq %%xmm1, %%xmm0")
FORM(o_pmuldq,  "pmuldq  %%xmm1, %%xmm0")

FORM(o_packsswb, "packsswb %%xmm1, %%xmm0")
FORM(o_packssdw, "packssdw %%xmm1, %%xmm0")
FORM(o_packuswb, "packuswb %%xmm1, %%xmm0")
FORM(o_packusdw, "packusdw %%xmm1, %%xmm0")

FORM(o_pmaddwd,  "pmaddwd  %%xmm1, %%xmm0")
FORM(o_psadbw,   "psadbw   %%xmm1, %%xmm0")

typedef Out (*Fn)(const uint64_t*, const uint64_t*);

enum Kind { K_SHIFT, K_WIDEN, K_BIN };

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
	{"paddsb",  o_paddsb,  K_BIN, 8},  {"paddsw",  o_paddsw,  K_BIN, 16},
	{"paddusb", o_paddusb, K_BIN, 8},  {"paddusw", o_paddusw, K_BIN, 16},
	{"psubsb",  o_psubsb,  K_BIN, 8},  {"psubsw",  o_psubsw,  K_BIN, 16},
	{"psubusb", o_psubusb, K_BIN, 8},  {"psubusw", o_psubusw, K_BIN, 16},
	{"pmullw",  o_pmullw,  K_BIN, 16}, {"pmulhw",  o_pmulhw,  K_BIN, 16},
	{"pmulhuw", o_pmulhuw, K_BIN, 16}, {"pmulld",  o_pmulld,  K_BIN, 32},
	{"pmuludq", o_pmuludq, K_BIN, 32}, {"pmuldq",  o_pmuldq,  K_BIN, 32},
	{"packsswb", o_packsswb, K_BIN, 16}, {"packssdw", o_packssdw, K_BIN, 32},
	{"packuswb", o_packuswb, K_BIN, 16}, {"packusdw", o_packusdw, K_BIN, 32},
	{"pmaddwd",  o_pmaddwd,  K_BIN, 16}, {"psadbw",   o_psadbw,   K_BIN, 8},
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

/* A quadword whose every element is drawn from the edges of its range: the
   signed and unsigned extremes, their neighbours, zero and one. Saturation is
   a property of the edges, and uniform noise is almost never near them. */
static uint64_t edgy(unsigned elem)
{
	uint64_t mask = elem == 64 ? ~0ULL : ((1ULL << elem) - 1);
	uint64_t out = 0;
	for (unsigned off = 0; off < 64; off += elem) {
		uint64_t v;
		switch (random() % 8) {
			case 0: v = 0; break;
			case 1: v = 1; break;
			case 2: v = mask; break;                       /* -1, or the max */
			case 3: v = mask >> 1; break;                  /* signed max */
			case 4: v = (mask >> 1) + 1; break;            /* signed min */
			case 5: v = mask - 1; break;
			case 6: v = (uint64_t)(random()) & mask; break;
			default: v = (uint64_t)(-(int64_t)(random() % 4)) & mask; break;
		}
		out |= (v & mask) << off;
	}
	return out;
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
			} else if (TBL[k].kind == K_BIN) {
				/* Uniform noise almost never saturates: two random bytes sum
				   past 127 about a quarter of the time and past 255 almost
				   never. Half the rows are drawn from the edges of the
				   element range instead, so the clamp is actually exercised
				   rather than merely present. */
				a[0] = edgy(TBL[k].elem); a[1] = edgy(TBL[k].elem);
				b[0] = edgy(TBL[k].elem); b[1] = edgy(TBL[k].elem);
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
