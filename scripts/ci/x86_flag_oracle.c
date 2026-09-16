/**
 * @file scripts/ci/x86_flag_oracle.c
 * @brief Executes x86 arithmetic natively and reports result + flags.
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek, licensed under the MIT license
 *
 * Part of FLAG-01. See scripts/ci/check_x86_flags.sh.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t res; uint32_t flags; } Out;

#define FLAGMASK 0x8d5u   /* CF PF AF ZF SF OF */

#define OP2_64(name, insn)                                                    \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f;                                               \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "r"(b) : "cc");                     \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

OP2_64(o_add64, "addq %2, %0")
OP2_64(o_sub64, "subq %2, %0")
OP2_64(o_and64, "andq %2, %0")
OP2_64(o_or64,  "orq  %2, %0")
OP2_64(o_xor64, "xorq %2, %0")
OP2_64(o_cmp64, "cmpq %2, %0")
OP2_64(o_test64,"testq %2, %0")
OP2_64(o_imul64,"imulq %2, %0")

#define OP2_32(name, insn)                                                    \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint32_t r = (uint32_t)a; uint64_t f;                                     \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "r"((uint32_t)b) : "cc");           \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

OP2_32(o_add32, "addl %2, %0")
OP2_32(o_sub32, "subl %2, %0")
OP2_32(o_imul32,"imull %2, %0")

#define OP1_64(name, insn)                                                    \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f; (void)b;                                      \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) :: "cc");                             \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

OP1_64(o_neg64, "negq %0")
OP1_64(o_inc64, "incq %0")
OP1_64(o_dec64, "decq %0")
OP1_64(o_not64, "notq %0")

/* shifts: count in cl */
#define SH64(name, insn)                                                      \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f; uint8_t c = (uint8_t)b;                       \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "c"(c) : "cc");                     \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

SH64(o_shl64, "shlq %%cl, %0")
SH64(o_shr64, "shrq %%cl, %0")
SH64(o_sar64, "sarq %%cl, %0")
SH64(o_rol64, "rolq %%cl, %0")
SH64(o_ror64, "rorq %%cl, %0")

/* With-carry forms: the incoming CF comes from bit 0 of b, so both values get
   exercised. The translator side is set up identically. */
#define CARRY2_64(name, insn)                                                 \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f; uint64_t cin = b & 1;                         \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\torq %3, (%%rsp)\n\tpopfq\n\t" \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "r"(b), "r"(cin) : "cc");           \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

CARRY2_64(o_adc64, "adcq %2, %0")
CARRY2_64(o_sbb64, "sbbq %2, %0")

#define BT2_64(name, insn)                                                    \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f;                                               \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "r"(b) : "cc");                     \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }

BT2_64(o_bt64,  "btq  %2, %0")
BT2_64(o_bts64, "btsq %2, %0")
BT2_64(o_btr64, "btrq %2, %0")
BT2_64(o_btc64, "btcq %2, %0")

#define RC64(name, insn)                                                      \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = a; uint64_t f; uint8_t c = (uint8_t)b; uint64_t cin = b & 1; \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\torq %3, (%%rsp)\n\tpopfq\n\t" \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "c"(c), "r"(cin) : "cc");           \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }
RC64(o_rcl64, "rclq %%cl, %0")
RC64(o_rcr64, "rcrq %%cl, %0")

#define BS64(name, insn)                                                      \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint64_t r = 0; uint64_t f; (void)a;                                      \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "=r"(r), "=r"(f) : "r"(b) : "cc");                     \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }
BS64(o_bsf64, "bsfq %2, %0")
BS64(o_bsr64, "bsrq %2, %0")

#define OP2_8(name, insn)                                                     \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint8_t r = (uint8_t)a; uint64_t f;                                       \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+q"(r), "=r"(f) : "q"((uint8_t)b) : "cc");            \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }
OP2_8(o_add8, "addb %2, %0")
OP2_8(o_sub8, "subb %2, %0")

#define OP2_16(name, insn)                                                    \
  static Out name(uint64_t a, uint64_t b) {                                   \
    uint16_t r = (uint16_t)a; uint64_t f;                                     \
    __asm__ volatile("pushfq\n\tandq $0, (%%rsp)\n\tpopfq\n\t"                \
                     insn "\n\tpushfq\n\tpop %1"                              \
                     : "+r"(r), "=r"(f) : "r"((uint16_t)b) : "cc");           \
    Out o = { r, (uint32_t)f & FLAGMASK }; return o;                          \
  }
OP2_16(o_add16, "addw %2, %0")
OP2_16(o_sub16, "subw %2, %0")

typedef Out (*Fn)(uint64_t, uint64_t);
static struct { const char* name; const char* asmtext; Fn fn; unsigned bits; } TBL[] = {
  {"add64", "add rax, rcx",  o_add64, 64}, {"sub64", "sub rax, rcx", o_sub64, 64},
  {"and64", "and rax, rcx",  o_and64, 64}, {"or64",  "or rax, rcx",  o_or64,  64},
  {"xor64", "xor rax, rcx",  o_xor64, 64}, {"cmp64", "cmp rax, rcx", o_cmp64, 64},
  {"test64","test rax, rcx", o_test64,64}, {"imul64","imul rax, rcx",o_imul64,64},
  {"add32", "add eax, ecx",  o_add32, 32}, {"sub32", "sub eax, ecx", o_sub32, 32},
  {"imul32","imul eax, ecx", o_imul32,32},
  {"neg64", "neg rax",       o_neg64, 64}, {"inc64", "inc rax",      o_inc64, 64},
  {"dec64", "dec rax",       o_dec64, 64}, {"not64", "not rax",      o_not64, 64},
  {"shl64", "shl rax, cl",   o_shl64, 64}, {"shr64", "shr rax, cl",  o_shr64, 64},
  {"sar64", "sar rax, cl",   o_sar64, 64}, {"rol64", "rol rax, cl",  o_rol64, 64},
  {"ror64", "ror rax, cl",   o_ror64, 64},
  {"adc64", "adc rax, rcx",  o_adc64, 64}, {"sbb64", "sbb rax, rcx", o_sbb64, 64},
  {"bt64",  "bt rax, rcx",   o_bt64,  64}, {"bts64", "bts rax, rcx", o_bts64, 64},
  {"btr64", "btr rax, rcx",  o_btr64, 64}, {"btc64", "btc rax, rcx", o_btc64, 64},
  {"rcl64", "rcl rax, cl",   o_rcl64, 64}, {"rcr64", "rcr rax, cl",  o_rcr64, 64},
  {"bsf64", "bsf rax, rcx",  o_bsf64, 64}, {"bsr64", "bsr rax, rcx", o_bsr64, 64},
  {"add8",  "add al, cl",    o_add8,   8}, {"sub8",  "sub al, cl",   o_sub8,   8},
  {"add16", "add ax, cx",    o_add16, 16}, {"sub16", "sub ax, cx",   o_sub16, 16},
};

/* random() returns 31 bits, so `random() << 32 ^ random()` leaves bit 31 and
   bit 63 clear in every draw. That is the whole signed half of the operand
   space unreachable, on a test whose job is to find sign bugs. */
static uint64_t rnd64(void)
{
  uint64_t x = 0;
  for (int i = 0; i < 5; ++i) { x = (x << 13) ^ (uint64_t)(random() & 0x1fff); }
  return x;
}

int main(int argc, char** argv)
{
  long n = argc > 1 ? atol(argv[1]) : 200;
  srandom(argc > 2 ? atol(argv[2]) : 20240101);
  printf("# name asmtext bits a b res cf pf af zf sf of\n");
  for (size_t k = 0; k < sizeof TBL / sizeof TBL[0]; ++k) {
    for (long i = 0; i < n; ++i) {
      uint64_t a, b;
      int mode = random() % 4;
      if (mode == 0) { a = random() % 8; b = random() % 8; }
      else if (mode == 1) { a = rnd64(); b = rnd64(); }
      else if (mode == 2) { a = 1ULL << (random() % 64); b = 1ULL << (random() % 64); }
      else { a = ~0ULL >> (random() % 64); b = ~0ULL >> (random() % 64); }
      Out o = TBL[k].fn(a, b);
      printf("%s|%s|%u|%llu|%llu|%llu|%u|%u|%u|%u|%u|%u\n",
             TBL[k].name, TBL[k].asmtext, TBL[k].bits,
             (unsigned long long)a, (unsigned long long)b, (unsigned long long)o.res,
             (o.flags >> 0) & 1, (o.flags >> 2) & 1, (o.flags >> 4) & 1,
             (o.flags >> 6) & 1, (o.flags >> 7) & 1, (o.flags >> 11) & 1);
    }
  }
  return 0;
}
