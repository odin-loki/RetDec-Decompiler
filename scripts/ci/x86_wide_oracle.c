/**
 * @file scripts/ci/x86_wide_oracle.c
 * @brief Executes the two-register-result x86 forms natively: MUL, IMUL,
 *        DIV, IDIV, SHLD and SHRD.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 *
 * x86_flag_oracle.c covers the instructions whose whole answer is one
 * register and six flags. These are the ones it cannot express:
 *
 *   MUL/IMUL  write a result twice as wide as their operand, split across
 *             two registers, and at 8 and 16 bits they write only PART of
 *             those registers -- `mul cl` writes AX and leaves RAX[63:16]
 *             alone. Every row therefore carries the FULL 64-bit RAX and RDX
 *             before and after, so a translator that zeroes what it should
 *             merge is a mismatch rather than something nobody looked at.
 *   DIV/IDIV  read a dividend twice as wide as their operand and write both
 *             a quotient and a remainder. Operands are built backwards --
 *             from a quotient, a divisor and a remainder -- so the dividend
 *             is in range by construction and the CPU never takes #DE.
 *   SHLD/SHRD have three operands, and the shift count is masked by the
 *             OPERAND size, not by the mode: `shld eax, ecx, cl` with cl=40
 *             shifts by 8.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t a, d; uint32_t flags; int ok; } Out;

#define FLAGMASK 0x8d5u   /* CF PF AF ZF SF OF */

#define CLEAR "pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"
#define SAVE  "pushfq\n\tpop %2"

/* ---------------------------------------------------------------- MUL/IMUL */

#define MUL_FORM(name, insn)                                                  \
  static Out name(uint64_t a, uint64_t b, uint64_t d) {                       \
    uint64_t ra = a, rd = d, f;                                               \
    __asm__ volatile(CLEAR insn "\n\t" SAVE                                   \
                     : "+a"(ra), "+d"(rd), "=r"(f) : "c"(b) : "cc");          \
    Out o = { ra, rd, (uint32_t)f & FLAGMASK, 1 }; return o;                  \
  }

MUL_FORM(o_mul8,   "mulb  %%cl")
MUL_FORM(o_mul16,  "mulw  %%cx")
MUL_FORM(o_mul32,  "mull  %%ecx")
MUL_FORM(o_mul64,  "mulq  %%rcx")
MUL_FORM(o_imul8,  "imulb %%cl")
MUL_FORM(o_imul16, "imulw %%cx")
MUL_FORM(o_imul32, "imull %%ecx")
MUL_FORM(o_imul64, "imulq %%rcx")

/* ---------------------------------------------------------------- DIV/IDIV */

/* The dividend is built from a quotient, a divisor and a remainder rather
   than drawn at random, so that it is representable by construction. A random
   dividend would take #DE -- an unmaskable fault, not a wrong answer -- for
   most of the interesting divisors, and a harness that crashes teaches
   nothing. Each row is still checked with C arithmetic before it is used. */

MUL_FORM(o_div8,   "divb  %%cl")
MUL_FORM(o_div16,  "divw  %%cx")
MUL_FORM(o_div32,  "divl  %%ecx")
MUL_FORM(o_div64,  "divq  %%rcx")
MUL_FORM(o_idiv8,  "idivb %%cl")
MUL_FORM(o_idiv16, "idivw %%cx")
MUL_FORM(o_idiv32, "idivl %%ecx")
MUL_FORM(o_idiv64, "idivq %%rcx")

/* ---------------------------------------------------------------- SHLD/SHRD */

/* The count is CL, which is the low byte of the source register. Both sides
   of the comparison use the same register, so this is a property of the test
   rather than a restriction: it means one value in the row drives both. */

#define SH3(name, insn)                                                       \
  static Out name(uint64_t a, uint64_t b, uint64_t d) {                       \
    uint64_t ra = a, f; (void)d;                                              \
    __asm__ volatile(CLEAR insn "\n\t" SAVE                                   \
                     : "+a"(ra), "+c"(b), "=r"(f) :: "cc");                   \
    Out o = { ra, d, (uint32_t)f & FLAGMASK, 1 }; return o;                   \
  }

SH3(o_shld16, "shldw %%cl, %%cx,  %%ax")
SH3(o_shld32, "shldl %%cl, %%ecx, %%eax")
SH3(o_shld64, "shldq %%cl, %%rcx, %%rax")
SH3(o_shrd16, "shrdw %%cl, %%cx,  %%ax")
SH3(o_shrd32, "shrdl %%cl, %%ecx, %%eax")
SH3(o_shrd64, "shrdq %%cl, %%rcx, %%rax")

/* ------------------------------------------- single-operand shift / rotate */

/* These are already covered at 64 bits by x86_flag_oracle.c, where the whole
   answer fits in one register. At 32 bits they are not, and 32 bits is where
   the two interesting things happen: the count is masked to five bits rather
   than six, and the destination write zeroes the upper half of the 64-bit
   register even when the count masks to zero. The incoming carry comes from
   bit 8 of the count word so that RCL and RCR see both values. */

#define SH1(name, insn)                                                       \
  static Out name(uint64_t a, uint64_t b, uint64_t d) {                       \
    uint64_t ra = a, f, cin = (b >> 8) & 1; (void)d;                          \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\torq %3, (%%rsp)\n\tpopfq\n\t" \
                     insn "\n\t" SAVE                                         \
                     : "+a"(ra), "+c"(b), "=r"(f) : "r"(cin) : "cc");         \
    Out o = { ra, d, (uint32_t)f & FLAGMASK, 1 }; return o;                   \
  }

SH1(o_shl32, "shll %%cl, %%eax")
SH1(o_shr32, "shrl %%cl, %%eax")
SH1(o_sar32, "sarl %%cl, %%eax")
SH1(o_rol32, "roll %%cl, %%eax")
SH1(o_ror32, "rorl %%cl, %%eax")
SH1(o_rcl32, "rcll %%cl, %%eax")
SH1(o_rcr32, "rcrl %%cl, %%eax")
SH1(o_shl8,  "shlb %%cl, %%al")
SH1(o_shr8,  "shrb %%cl, %%al")
SH1(o_rol8,  "rolb %%cl, %%al")
SH1(o_ror8,  "rorb %%cl, %%al")
SH1(o_shl16, "shlw %%cl, %%ax")
SH1(o_sar16, "sarw %%cl, %%ax")
SH1(o_rol16, "rolw %%cl, %%ax")

/* ------------------------------------------------------------- generation */

typedef Out (*Fn)(uint64_t, uint64_t, uint64_t);

enum Kind { K_MUL, K_DIV, K_SH, K_SH1 };

static struct {
	const char* name; Fn fn; unsigned bits; enum Kind kind; int sign;
} TBL[] = {
	{"mul8",   o_mul8,    8, K_MUL, 0}, {"mul16",  o_mul16,  16, K_MUL, 0},
	{"mul32",  o_mul32,  32, K_MUL, 0}, {"mul64",  o_mul64,  64, K_MUL, 0},
	{"imul8",  o_imul8,   8, K_MUL, 1}, {"imul16", o_imul16, 16, K_MUL, 1},
	{"imul32", o_imul32, 32, K_MUL, 1}, {"imul64", o_imul64, 64, K_MUL, 1},
	{"div8",   o_div8,    8, K_DIV, 0}, {"div16",  o_div16,  16, K_DIV, 0},
	{"div32",  o_div32,  32, K_DIV, 0}, {"div64",  o_div64,  64, K_DIV, 0},
	{"idiv8",  o_idiv8,   8, K_DIV, 1}, {"idiv16", o_idiv16, 16, K_DIV, 1},
	{"idiv32", o_idiv32, 32, K_DIV, 1}, {"idiv64", o_idiv64, 64, K_DIV, 1},
	{"shld16", o_shld16, 16, K_SH,  0}, {"shld32", o_shld32, 32, K_SH,  0},
	{"shld64", o_shld64, 64, K_SH,  0},
	{"shrd16", o_shrd16, 16, K_SH,  0}, {"shrd32", o_shrd32, 32, K_SH,  0},
	{"shrd64", o_shrd64, 64, K_SH,  0},
	{"shl32",  o_shl32,  32, K_SH1, 0}, {"shr32",  o_shr32,  32, K_SH1, 0},
	{"sar32",  o_sar32,  32, K_SH1, 0}, {"rol32",  o_rol32,  32, K_SH1, 0},
	{"ror32",  o_ror32,  32, K_SH1, 0}, {"rcl32",  o_rcl32,  32, K_SH1, 0},
	{"rcr32",  o_rcr32,  32, K_SH1, 0},
	{"shl8",   o_shl8,    8, K_SH1, 0}, {"shr8",   o_shr8,    8, K_SH1, 0},
	{"rol8",   o_rol8,    8, K_SH1, 0}, {"ror8",   o_ror8,    8, K_SH1, 0},
	{"shl16",  o_shl16,  16, K_SH1, 0}, {"sar16",  o_sar16,  16, K_SH1, 0},
	{"rol16",  o_rol16,  16, K_SH1, 0},
};

/* SHL, SHR and SAR leave the destination AND CF undefined when the masked
   count is at least the operand width, so at 8 and 16 bits the count is kept
   below the width. The rotates are defined for every count -- a rotate by
   more than its width is just a rotate -- so they get the whole byte, which
   is where the masking is actually tested. */
static int countMustBeSmall(const char* nm)
{
	return (nm[0] == 's')
	    && (nm[strlen(nm) - 1] == '8'
	        || (nm[strlen(nm) - 2] == '1' && nm[strlen(nm) - 1] == '6'));
}

/* random() returns 31 bits, so the obvious `random() << 32 ^ random()` leaves
   bit 31 and bit 63 clear in EVERY draw -- 200,000 of them, checked. A
   generator that cannot produce a negative operand cannot find a sign bug,
   and this one was hiding IDIV's: the divisor was never negative, so the
   first sweep of these instructions came back clean on an instruction that
   gets `10 / -3` wrong. Five 13-bit chunks cover all 64. */
static uint64_t rnd64(void)
{
	uint64_t x = 0;
	for (int i = 0; i < 5; ++i) {
		x = (x << 13) ^ (uint64_t)(random() & 0x1fff);
	}
	return x;
}

/* A spread of shapes rather than uniform noise: small values, single bits,
   all-ones runs and the signed extremes are where the sign handling and the
   overflow flags actually differ. */
static uint64_t shaped(unsigned bits)
{
	uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
	switch (random() % 6) {
		case 0:  return (uint64_t)(random() % 8);
		case 1:  return rnd64() & mask;
		case 2:  return (1ULL << (random() % bits)) & mask;
		case 3:  return (~0ULL >> (random() % 64)) & mask;
		case 4:  return (uint64_t)(-(int64_t)(random() % 8)) & mask;
		default: return (1ULL << (bits - 1)) & mask;   /* the signed minimum */
	}
}

static uint64_t truncTo(uint64_t v, unsigned bits)
{
	return bits == 64 ? v : (v & ((1ULL << bits) - 1));
}

static int64_t sextFrom(uint64_t v, unsigned bits)
{
	if (bits == 64) return (int64_t)v;
	return (int64_t)(v << (64 - bits)) >> (64 - bits);
}

/* Build a dividend that the instruction can represent, and confirm it with C
   arithmetic before emitting the row. Returns 0 if this draw cannot be used. */
static int makeDividend(unsigned bits, int sign, uint64_t* lo, uint64_t* hi,
                        uint64_t divisor)
{
	if (truncTo(divisor, bits) == 0) return 0;

	if (sign) {
		__int128 dv = sextFrom(divisor, bits);
		__int128 q  = sextFrom(shaped(bits), bits);
		__int128 r  = (__int128)(random() % (dv < 0 ? -dv : dv));
		if (q < 0 || (q == 0 && (random() & 1))) r = -r;
		__int128 n = q * dv + r;
		/* Confirm: x86 truncates toward zero, and so does C99. */
		if (n / dv != q) return 0;
		{
			__int128 top = ((__int128)1 << (bits - 1)) - 1;
			__int128 bot = -((__int128)1 << (bits - 1));
			if (q > top || q < bot) return 0;
		}
		*lo = truncTo((uint64_t)(unsigned __int128)n, bits);
		*hi = bits == 8 ? 0
		    : truncTo((uint64_t)(((unsigned __int128)n) >> bits), bits);
		if (bits == 8) *lo = (uint64_t)(uint16_t)(unsigned __int128)n;
		return 1;
	} else {
		unsigned __int128 dv = truncTo(divisor, bits);
		unsigned __int128 q  = truncTo(shaped(bits), bits);
		unsigned __int128 r  = (unsigned __int128)((uint64_t)rnd64() % (uint64_t)dv);
		unsigned __int128 n  = q * dv + r;
		if (n / dv != q) return 0;
		*lo = truncTo((uint64_t)n, bits);
		*hi = bits == 8 ? 0 : truncTo((uint64_t)(n >> bits), bits);
		if (bits == 8) *lo = (uint64_t)(uint16_t)n;
		return 1;
	}
}

int main(int argc, char** argv)
{
	long n = argc > 1 ? atol(argv[1]) : 200;
	srandom(argc > 2 ? (unsigned)atol(argv[2]) : 20240202u);

	printf("# name bits a b d resA resD cf pf af zf sf of\n");
	for (size_t k = 0; k < sizeof TBL / sizeof TBL[0]; ++k) {
		long made = 0;
		for (long attempt = 0; attempt < n * 40 && made < n; ++attempt) {
			uint64_t a = rnd64(), b = rnd64(), d = rnd64();
			unsigned bits = TBL[k].bits;

			if (TBL[k].kind == K_DIV) {
				uint64_t lo, hi;
				b = (b & ~truncTo(~0ULL, bits)) | truncTo(shaped(bits), bits);
				if (!makeDividend(bits, TBL[k].sign, &lo, &hi, b)) continue;
				/* Keep whatever noise is in the bits the instruction does
				   not read, so a translator that reads too much shows up. */
				if (bits == 8) {
					a = (a & ~0xffffULL) | lo;      /* the dividend is AX */
				} else {
					a = bits == 64 ? lo : ((a & ~truncTo(~0ULL, bits)) | lo);
					d = bits == 64 ? hi : ((d & ~truncTo(~0ULL, bits)) | hi);
				}
			} else if (TBL[k].kind == K_SH1) {
				a = shaped(64);
				unsigned cnt = countMustBeSmall(TBL[k].name)
						? (unsigned)(random() % bits)
						: (unsigned)(random() % 256);
				b = (rnd64() & ~0xffULL) | cnt;
			} else if (TBL[k].kind == K_SH) {
				a = shaped(64); b = shaped(64);
				/* At 16 bits a count above 15 is architecturally undefined,
				   so it is not asked for. At 32 and 64 the whole byte is
				   fair game and is exactly where the masking is tested. */
				unsigned cnt = bits == 16 ? (unsigned)(random() % 16)
				                          : (unsigned)(random() % 256);
				b = (b & ~0xffULL) | cnt;
			} else {
				a = (a & ~truncTo(~0ULL, bits)) | truncTo(shaped(bits), bits);
				b = (b & ~truncTo(~0ULL, bits)) | truncTo(shaped(bits), bits);
			}

			Out o = TBL[k].fn(a, b, d);
			if (!o.ok) continue;
			printf("%s|%u|%llu|%llu|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u\n",
			       TBL[k].name, bits,
			       (unsigned long long)a, (unsigned long long)b,
			       (unsigned long long)d,
			       (unsigned long long)o.a, (unsigned long long)o.d,
			       (o.flags >> 0) & 1, (o.flags >> 2) & 1, (o.flags >> 4) & 1,
			       (o.flags >> 6) & 1, (o.flags >> 7) & 1, (o.flags >> 11) & 1);
			++made;
		}
	}
	return 0;
}
