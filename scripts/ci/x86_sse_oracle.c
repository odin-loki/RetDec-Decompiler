/**
 * @file scripts/ci/x86_sse_oracle.c
 * @brief Executes the x86 instructions that READ an XMM register and write a
 *        general-purpose register or the flags.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 *
 * The other two oracles pass their operands in general-purpose registers, so
 * neither can reach these. This one is the floating-point comparisons, the
 * sign-mask extractions and the scalar float-to-integer conversions -- the
 * instructions behind every `if (x < y)` and every `(int)x` in compiled
 * floating-point code.
 *
 * The VEX forms are here next to the SSE ones deliberately. `vucomisd` is
 * what a compiler emits for a double comparison on any machine built this
 * decade, and RetDec dispatched all four V-forms to nullptr, so an AVX binary
 * got a pseudo-assembly call where an SSE binary got a comparison.
 *
 * Operands are float BIT PATTERNS, not values: NaN (quiet and signalling),
 * both zeroes, both infinities, denormals, the integer-conversion boundaries
 * and ordinary numbers. The comparison flag table has a row for "unordered"
 * that nothing but a NaN reaches.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t gpr; uint32_t flags; } Out;

#define FLAGMASK 0x8d5u   /* CF PF AF ZF SF OF */
#define GPR_SEED 0xdeadbeefdeadbeefULL

/* The destination GPR is seeded with a recognisable pattern so that a 32-bit
   write failing to clear bits 63:32 is visible rather than accidentally
   right. */
#define FORM(name, insn)                                                      \
  static Out name(const uint64_t* a, const uint64_t* b) {                     \
    uint64_t g = GPR_SEED, f;                                                 \
    __asm__ volatile("movdqu %2, %%xmm0\n\t"                                  \
                     "movdqu %3, %%xmm1\n\t"                                  \
                     "pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\t"                                              \
                     "pushfq\n\tpop %1"                                       \
                     : "+a"(g), "=r"(f)                                       \
                     : "m"(*a), "m"(*b)                                       \
                     : "cc", "xmm0", "xmm1");                                 \
    Out o = { g, (uint32_t)f & FLAGMASK }; return o;                          \
  }

FORM(o_ucomisd,   "ucomisd  %%xmm1, %%xmm0")
FORM(o_ucomiss,   "ucomiss  %%xmm1, %%xmm0")
FORM(o_comisd,    "comisd   %%xmm1, %%xmm0")
FORM(o_comiss,    "comiss   %%xmm1, %%xmm0")
FORM(o_vucomisd,  "vucomisd %%xmm1, %%xmm0")
FORM(o_vucomiss,  "vucomiss %%xmm1, %%xmm0")
FORM(o_vcomisd,   "vcomisd  %%xmm1, %%xmm0")
FORM(o_vcomiss,   "vcomiss  %%xmm1, %%xmm0")

FORM(o_movmskps,  "movmskps  %%xmm1, %%eax")
FORM(o_movmskpd,  "movmskpd  %%xmm1, %%eax")
FORM(o_vmovmskps, "vmovmskps %%xmm1, %%eax")
FORM(o_vmovmskpd, "vmovmskpd %%xmm1, %%eax")

FORM(o_cvtsd2si,   "cvtsd2si   %%xmm1, %%eax")
FORM(o_cvtss2si,   "cvtss2si   %%xmm1, %%eax")
FORM(o_cvttsd2si,  "cvttsd2si  %%xmm1, %%eax")
FORM(o_cvttss2si,  "cvttss2si  %%xmm1, %%eax")
FORM(o_vcvtsd2si,  "vcvtsd2si  %%xmm1, %%eax")
FORM(o_vcvtss2si,  "vcvtss2si  %%xmm1, %%eax")
FORM(o_vcvttsd2si, "vcvttsd2si %%xmm1, %%eax")
FORM(o_vcvttss2si, "vcvttss2si %%xmm1, %%eax")

typedef Out (*Fn)(const uint64_t*, const uint64_t*);

enum Kind { K_CMP, K_MSK, K_CVT };

static struct { const char* name; Fn fn; enum Kind kind; int isDouble; } TBL[] = {
	{"ucomisd",   o_ucomisd,   K_CMP, 1}, {"ucomiss",   o_ucomiss,   K_CMP, 0},
	{"comisd",    o_comisd,    K_CMP, 1}, {"comiss",    o_comiss,    K_CMP, 0},
	{"vucomisd",  o_vucomisd,  K_CMP, 1}, {"vucomiss",  o_vucomiss,  K_CMP, 0},
	{"vcomisd",   o_vcomisd,   K_CMP, 1}, {"vcomiss",   o_vcomiss,   K_CMP, 0},
	{"movmskps",  o_movmskps,  K_MSK, 0}, {"movmskpd",  o_movmskpd,  K_MSK, 1},
	{"vmovmskps", o_vmovmskps, K_MSK, 0}, {"vmovmskpd", o_vmovmskpd, K_MSK, 1},
	{"cvtsd2si",   o_cvtsd2si,   K_CVT, 1}, {"cvtss2si",   o_cvtss2si,   K_CVT, 0},
	{"cvttsd2si",  o_cvttsd2si,  K_CVT, 1}, {"cvttss2si",  o_cvttss2si,  K_CVT, 0},
	{"vcvtsd2si",  o_vcvtsd2si,  K_CVT, 1}, {"vcvtss2si",  o_vcvtss2si,  K_CVT, 0},
	{"vcvttsd2si", o_vcvttsd2si, K_CVT, 1}, {"vcvttss2si", o_vcvttss2si, K_CVT, 0},
};

/* Bit patterns, not arithmetic. A generator that produces only ordinary
   numbers never reaches the row of the comparison table that matters. */
static const uint64_t D_POOL[] = {
	0x0000000000000000ULL, /* +0.0 */
	0x8000000000000000ULL, /* -0.0 */
	0x3ff0000000000000ULL, /* 1.0 */
	0xbff0000000000000ULL, /* -1.0 */
	0x4000000000000000ULL, /* 2.0 */
	0x7ff0000000000000ULL, /* +inf */
	0xfff0000000000000ULL, /* -inf */
	0x7ff8000000000000ULL, /* quiet NaN */
	0x7ff0000000000001ULL, /* signalling NaN */
	0x0000000000000001ULL, /* smallest denormal */
	0x41dfffffffe00000ULL, /* 2147483647.0, the largest int32 */
	0x41e0000000000000ULL, /* 2147483648.0, one past it */
	0xc1e0000000000000ULL, /* -2147483648.0, the smallest int32 */
	0xc1e0000000200000ULL, /* one below that */
	0x4330000000000000ULL, /* 2^52 */
	0x3fe0000000000000ULL, /* 0.5 -- rounds to even, not away */
	0x3ff8000000000000ULL, /* 1.5 */
	0x4008000000000000ULL, /* 3.0 */
	0x400c000000000000ULL, /* 3.5 */
	0x44e0000000000000ULL, /* 1.0e24, far out of int32 range */
};

static const uint32_t S_POOL[] = {
	0x00000000u, 0x80000000u,             /* +0.0, -0.0 */
	0x3f800000u, 0xbf800000u,             /* 1.0, -1.0 */
	0x40000000u,                          /* 2.0 */
	0x7f800000u, 0xff800000u,             /* +inf, -inf */
	0x7fc00000u, 0x7f800001u,             /* quiet NaN, signalling NaN */
	0x00000001u,                          /* smallest denormal */
	0x4effffffu,                          /* 2147483520.0 */
	0x4f000000u,                          /* 2147483648.0, one past int32 */
	0xcf000000u,                          /* -2147483648.0 */
	0xcf000001u,                          /* one below that */
	0x3f000000u, 0x3fc00000u,             /* 0.5, 1.5 */
	0x40400000u, 0x40600000u,             /* 3.0, 3.5 */
	0x6a000000u,                          /* 3.9e25, far out of range */
};

int main(int argc, char** argv)
{
	long n = argc > 1 ? atol(argv[1]) : 200;
	srandom(argc > 2 ? (unsigned)atol(argv[2]) : 20240303u);

	size_t nd = sizeof D_POOL / sizeof D_POOL[0];
	size_t ns = sizeof S_POOL / sizeof S_POOL[0];

	printf("# name x0lo x0hi x1lo x1hi gpr cf pf af zf sf of\n");
	for (size_t k = 0; k < sizeof TBL / sizeof TBL[0]; ++k) {
		int dbl = TBL[k].isDouble;
		/* Every ordered pair from the pool first -- the comparison table is
		   small enough to cover exhaustively, and exhaustive beats sampled
		   when the interesting rows are the rare ones. Random pairs after
		   that, to fill out whatever `n` asks for. */
		size_t pool = dbl ? nd : ns;
		long made = 0;
		for (size_t p = 0; p < pool * pool + (size_t)n; ++p) {
			uint64_t a[2], b[2];
			size_t ia, ib;
			if (p < pool * pool) { ia = p / pool; ib = p % pool; }
			else { ia = (size_t)(random() % (long)pool); ib = (size_t)(random() % (long)pool); }
			if (dbl) {
				a[0] = D_POOL[ia]; b[0] = D_POOL[ib];
			} else {
				a[0] = (uint64_t)S_POOL[ia] | ((uint64_t)S_POOL[(ia + 1) % pool] << 32);
				b[0] = (uint64_t)S_POOL[ib] | ((uint64_t)S_POOL[(ib + 1) % pool] << 32);
			}
			/* The upper half is filled with noise: these instructions read
			   lane 0 only, and a translator that reads more shows up. For
			   MOVMSKPS/PD it is real input -- all four (or two) lanes count. */
			a[1] = ((uint64_t)(uint32_t)random() << 32) ^ (uint32_t)random();
			b[1] = ((uint64_t)(uint32_t)random() << 32) ^ (uint32_t)random();
			if (TBL[k].kind == K_MSK) {
				b[1] = dbl ? D_POOL[(ib + 3) % pool]
				           : ((uint64_t)S_POOL[(ib + 2) % pool]
				              | ((uint64_t)S_POOL[(ib + 3) % pool] << 32));
			}

			Out o = TBL[k].fn(a, b);
			printf("%s|%llu|%llu|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u\n",
			       TBL[k].name,
			       (unsigned long long)a[0], (unsigned long long)a[1],
			       (unsigned long long)b[0], (unsigned long long)b[1],
			       (unsigned long long)o.gpr,
			       (o.flags >> 0) & 1, (o.flags >> 2) & 1, (o.flags >> 4) & 1,
			       (o.flags >> 6) & 1, (o.flags >> 7) & 1, (o.flags >> 11) & 1);
			++made;
		}
		(void)made;
	}
	return 0;
}
